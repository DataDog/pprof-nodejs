/*
 * Copyright 2026 Datadog, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Serializes normalized profiles as OpenTelemetry profiles, in an
 * ExportProfilesServiceRequest of pprof-format/otel.
 *
 * The request has one profile per sample type, since an OTel profile has a
 * single sample type, and all of them share one dictionary. A sample count
 * is the exception when every sampling event is a single timestamped
 * observation (time profiles normalized with samplePerContext): it is then
 * folded into the profile of the period type, whose observations include
 * every sampling event, even zero-valued ones such as idle samples' wall
 * time. Otherwise the count keeps a profile of its own, so no count is lost.
 *
 * Samples with the same stack, attributes and link are merged into one, as
 * the OTel format requires: timestamped samples accumulate a value and a
 * timestamp per observation, and samples without a timestamp are summed into
 * one value. Other zero values are left out of the profile of their sample
 * type: e.g. zero CPU time, or the wall time of the synthetic non-JS threads
 * sample.
 *
 * OtelProfilesBuilder collects several profiles, e.g. time and heap, into one
 * request; serializeTimeProfileOtel() and serializeHeapProfileOtel() are
 * shorthands for a request with a single profile.
 *
 * Frames whose script declares a source map that can't be found locally, and
 * so are candidates for server-side unminification, are marked with a
 * location attribute, and their profiles with a profile attribute. (pprof
 * carries this as a profile comment, which OTel reserves for human-readable
 * text.)
 */

import {
  AnyValueInput,
  ExportProfilesServiceRequest,
  FunctionInput,
  InstrumentationScopeInput,
  KeyValueAndUnitInput,
  KeyValueInput,
  LinkInput,
  LocationInput,
  ProfileInput,
  SampleInput,
  StackInput,
  StringTable,
} from 'pprof-format/otel';
import {
  Frame,
  frameFunctionName,
  HeapProfileNormalizerOptions,
  ProfileMetadata,
  ProfileVisitor,
  NormalizedSample,
  SampleType,
  SampleLabel,
  TimeProfileNormalizerOptions,
  visitHeapProfile,
  visitTimeProfile,
} from './profile-normalizer';
import {
  AllocationProfileNode,
  AllocationProfileNodeWithStats,
  TimeProfile,
} from './v8-types';

/**
 * Trace and span IDs that link a sample to a span. The builder copies them,
 * and treats a link whose trace ID or span ID is all zeros, which is invalid,
 * as no link.
 */
export interface SampleLink {
  /** 16 bytes. */
  traceId: Uint8Array;
  /** 8 bytes. */
  spanId: Uint8Array;
}

/**
 * Turns some of a sample's labels into a link to a span. Returns the link, if
 * any, and the labels to keep as the sample's attributes.
 */
export type LinkExtractor = (labels: SampleLabel[]) => {
  link?: SampleLink;
  labels: SampleLabel[];
};

export interface OtelSerializerOptions {
  /** Attributes of the resource that produced the profile. */
  resourceAttributes?: KeyValueInput[];
  /** The instrumentation scope. Defaults to `@datadog/pprof`. */
  scope?: InstrumentationScopeInput;
  /** Without one, all labels become attributes. */
  extractLink?: LinkExtractor;
}

const DEFAULT_SCOPE: InstrumentationScopeInput = {name: '@datadog/pprof'};

const SAMPLE_COUNT: SampleType = {type: 'sample', unit: 'count'};

/** The OTel profiles of one normalized profile, by its sample type index. */
interface ProfileLayout {
  profiles: ProfileInput[];
  /** Per profile: samples by identity, for merging. */
  samplesByIdentity: Array<Map<string, SampleInput>>;
  /**
   * Index of a sample count type that can be folded into the period type's
   * profile, or -1.
   */
  countIndex: number;
  /** Index of the period type. */
  periodIndex: number;
  /**
   * Whether the count can still be folded: every sampling event so far was a
   * single timestamped observation.
   */
  foldable: boolean;
}

// Boolean attributes marking frames with a missing source map, and profiles
// that have such frames. There is no semantic convention for this, so the keys
// are Datadog's own.
// TODO: confirm the keys with the Datadog profiling backend.
export const FRAME_MISSING_SOURCE_MAP = 'datadog.frame.missing_source_map';
export const PROFILE_HAS_MISSING_SOURCE_MAPS =
  'datadog.profile.has_missing_source_maps';

/**
 * Builds one ExportProfilesServiceRequest from any number of normalized
 * profiles, which share the request's dictionary: e.g. the time and heap
 * profiles of one collection cycle. Each profile is added through a visitor
 * from visitor(), so that whatever drives the visit (the normalizer, or a
 * profiler emitting samples itself) needs no knowledge of the OTel format.
 *
 * Functions, locations, stacks, attributes and links are deduplicated across
 * all profiles, and each dictionary table starts with its zero value.
 *
 * Visitors run synchronously, and build() can be called after any number of
 * completed visits. A visit that throws leaves partial data in the builder,
 * so discard the builder after a failed visit.
 */
export class OtelProfilesBuilder {
  private readonly strings = new StringTable();
  private readonly functionTable: FunctionInput[] = [{}];
  private readonly locationTable: LocationInput[] = [{}];
  private readonly stackTable: StackInput[] = [{}];
  private readonly attributeTable: KeyValueAndUnitInput[] = [{}];
  // An all-zero link, with IDs of the lengths codecs expect.
  private readonly linkTable: LinkInput[] = [
    {traceId: new Uint8Array(16), spanId: new Uint8Array(8)},
  ];
  private readonly functionIndices = new Map<string, number>();
  private readonly locationIndices = new Map<string, number>();
  private readonly stackIndices = new Map<string, number>();
  private readonly attributeIndices = new Map<string, number>();
  private readonly linkIndices = new Map<string, number>();
  private readonly profiles: ProfileInput[] = [];

  constructor(private readonly options: OtelSerializerOptions = {}) {}

  /**
   * Returns a visitor that adds one normalized profile to the request, as one
   * OTel profile per sample type. Use a new visitor for each profile.
   */
  visitor(): ProfileVisitor {
    let layout: ProfileLayout = {
      profiles: [],
      samplesByIdentity: [],
      countIndex: -1,
      periodIndex: -1,
      foldable: false,
    };
    return {
      metadata: metadata => {
        layout = this.layout(metadata);
        this.profiles.push(...layout.profiles);
      },
      frame: frame => this.locationIndex(frame),
      stack: frameIds => this.stackIndex(frameIds),
      sample: sample => this.addSample(sample, layout),
      end: ({hasMissingMapFiles}) => {
        let profiles = layout.profiles;
        if (layout.countIndex >= 0 && layout.foldable) {
          const count = profiles[layout.countIndex];
          this.profiles.splice(this.profiles.indexOf(count), 1);
          profiles = profiles.filter(profile => profile !== count);
        }
        if (hasMissingMapFiles) {
          const attribute = this.trueAttribute(PROFILE_HAS_MISSING_SOURCE_MAPS);
          for (const profile of profiles) {
            profile.attributeIndices = [attribute];
          }
        }
      },
    };
  }

  /**
   * Creates the OTel profiles for a normalized profile's sample types.
   *
   * A sample count type can be folded into the profile of the period type
   * (wall time, for time profiles) instead of keeping a profile of its own:
   * that profile gets an observation for every sampling event, even one whose
   * value is zero, so the count is its number of observations. That keeps
   * idle samples, which have zero wall time, as timestamped evidence that
   * sampling happened, without repeating every sample in a count profile.
   * The fold is only lossless if every sampling event is a single timestamped
   * observation, so the count profile is built anyway, and dropped at the end
   * of the visit only if that turned out to be the case.
   */
  private layout(metadata: ProfileMetadata): ProfileLayout {
    const {sampleTypes, periodType} = metadata;
    const sameType = (a: SampleType, b: SampleType) =>
      a.type === b.type && a.unit === b.unit;
    let countIndex = sampleTypes.findIndex(t => sameType(t, SAMPLE_COUNT));
    const periodIndex = sampleTypes.findIndex(t => sameType(t, periodType));
    if (periodIndex < 0 || periodIndex === countIndex) countIndex = -1;

    const otelPeriodType = this.valueType(periodType);
    const profiles = sampleTypes.map(sampleType => ({
      sampleType: this.valueType(sampleType),
      periodType: otelPeriodType,
      period: metadata.period,
      // Sample timestamps should lie within the profile's time range, so it
      // starts at the start of collection when that is known.
      timeUnixNano: metadata.startTimeNanos ?? metadata.timeNanos,
      durationNano: metadata.durationNanos ?? 0,
      samples: [],
    }));
    return {
      profiles,
      samplesByIdentity: profiles.map(() => new Map()),
      countIndex,
      periodIndex,
      foldable: countIndex >= 0,
    };
  }

  /** Builds the request from the profiles added so far. */
  build(): ExportProfilesServiceRequest {
    return new ExportProfilesServiceRequest({
      resourceProfiles: [
        {
          resource: {attributes: this.options.resourceAttributes ?? []},
          scopeProfiles: [
            {
              scope: this.options.scope ?? DEFAULT_SCOPE,
              profiles: this.profiles,
            },
          ],
        },
      ],
      dictionary: {
        functionTable: this.functionTable,
        locationTable: this.locationTable,
        linkTable: this.linkTable,
        stringTable: this.strings,
        attributeTable: this.attributeTable,
        stackTable: this.stackTable,
      },
    });
  }

  private addSample(sample: NormalizedSample, layout: ProfileLayout) {
    const {profiles, samplesByIdentity, countIndex, periodIndex} = layout;
    // With a foldable count, the period type's profile observes every
    // sampling event, even one with a zero value, such as an idle sample's
    // wall time.
    const count = countIndex >= 0 ? sample.values[countIndex] : 0;
    const isSamplingEvent = count > 0;
    // Folding is lossless only if the period type's observations are exactly
    // the sampling events, one each: every sampling event is a single
    // timestamped observation, and no other sample adds an observation.
    if (
      isSamplingEvent
        ? sample.timestamp === undefined || count !== 1
        : countIndex >= 0 && sample.values[periodIndex] !== 0
    ) {
      layout.foldable = false;
    }
    let labels = sample.labels;
    let linkIndex = 0;
    if (this.options.extractLink) {
      const extracted = this.options.extractLink(labels);
      labels = extracted.labels;
      if (extracted.link) {
        linkIndex = this.linkIndex(extracted.link);
      }
    }
    // Attributes form a set, so order them for the sample's identity.
    const attributeIndices = labels
      .map(label => this.attributeIndex(label))
      .sort((a, b) => a - b);
    const timestamped = sample.timestamp !== undefined;
    const identity = `${sample.stackId}|${attributeIndices.join(',')}|${linkIndex}|${timestamped}`;

    sample.values.forEach((value, i) => {
      const profile = profiles[i];
      if (value === 0 && !(i === periodIndex && isSamplingEvent)) return;
      const samples = samplesByIdentity[i];
      let otelSample = samples.get(identity);
      if (otelSample === undefined) {
        otelSample = {
          stackIndex: sample.stackId,
          attributeIndices,
          linkIndex,
          values: [],
          timestampsUnixNano: [],
        };
        samples.set(identity, otelSample);
        profile.samples!.push(otelSample);
      }
      if (timestamped) {
        otelSample.values!.push(value);
        otelSample.timestampsUnixNano!.push(sample.timestamp!);
      } else if (otelSample.values!.length === 0) {
        otelSample.values!.push(value);
      } else {
        otelSample.values![0] = Number(otelSample.values![0]) + value;
      }
    });
  }

  private valueType({type, unit}: {type: string; unit: string}) {
    return {
      typeStrindex: this.strings.dedup(type),
      unitStrindex: this.strings.dedup(unit),
    };
  }

  // Functions are identified by what they encode as: their display name and
  // resolved file. (After source mapping, one script can map to several
  // files, so a script ID doesn't identify a file.)
  private functionIndex(frame: Frame): number {
    const name = frameFunctionName(frame);
    const key = `${frame.file.length}:${frame.file}:${name}`;
    let index = this.functionIndices.get(key);
    if (index === undefined) {
      index =
        this.functionTable.push({
          nameStrindex: this.strings.dedup(name),
          filenameStrindex: this.strings.dedup(frame.file),
        }) - 1;
      this.functionIndices.set(key, index);
    }
    return index;
  }

  private locationIndex(frame: Frame): number {
    const functionIndex = this.functionIndex(frame);
    const line = frame.line ?? 0;
    const column = frame.column ?? 0;
    const key = `${functionIndex}:${line}:${column}:${frame.missingMapFile}`;
    let index = this.locationIndices.get(key);
    if (index === undefined) {
      index =
        this.locationTable.push({
          lines: [{functionIndex, line, column}],
          attributeIndices: frame.missingMapFile
            ? [this.trueAttribute(FRAME_MISSING_SOURCE_MAP)]
            : [],
        }) - 1;
      this.locationIndices.set(key, index);
    }
    return index;
  }

  private stackIndex(locationIndices: number[]): number {
    const key = locationIndices.join(',');
    let index = this.stackIndices.get(key);
    if (index === undefined) {
      index = this.stackTable.push({locationIndices}) - 1;
      this.stackIndices.set(key, index);
    }
    return index;
  }

  private attributeIndex([key, value]: SampleLabel): number {
    // JSON-encoding the tuple keeps the key unambiguous whatever the strings
    // contain.
    const internKey = JSON.stringify([key, typeof value, String(value)]);
    return this.internAttribute(internKey, () => ({
      keyStrindex: this.strings.dedup(key),
      value: anyValue(value),
    }));
  }

  private trueAttribute(key: string): number {
    return this.internAttribute(
      JSON.stringify([key, 'boolean', 'true']),
      () => ({
        keyStrindex: this.strings.dedup(key),
        value: {boolValue: true},
      }),
    );
  }

  private internAttribute(
    key: string,
    create: () => KeyValueAndUnitInput,
  ): number {
    let index = this.attributeIndices.get(key);
    if (index === undefined) {
      index = this.attributeTable.push(create()) - 1;
      this.attributeIndices.set(key, index);
    }
    return index;
  }

  private linkIndex(link: SampleLink): number {
    if (link.traceId.length !== 16 || link.spanId.length !== 8) {
      throw new TypeError(
        `A link needs a 16-byte trace ID and an 8-byte span ID, not ${link.traceId.length} and ${link.spanId.length} bytes`,
      );
    }
    // A trace or span ID of all zeros is invalid, so either one means no
    // link: index 0
    if (isAllZero(link.traceId) || isAllZero(link.spanId)) return 0;
    const key = `${hex(link.traceId)}:${hex(link.spanId)}`;
    let index = this.linkIndices.get(key);
    if (index === undefined) {
      // Copy the IDs, in case the caller reuses its arrays
      index =
        this.linkTable.push({
          traceId: Uint8Array.from(link.traceId),
          spanId: Uint8Array.from(link.spanId),
        }) - 1;
      this.linkIndices.set(key, index);
    }
    return index;
  }
}

/**
 * Converts a label value to an attribute value. Integers become int64
 * attributes: like pprof's int64 labels, bigints of 2^63 and above (such as
 * large unsigned 64-bit span IDs) wrap to negative values, and bits beyond 64
 * are lost. Labels whose exact unsigned value matters should be strings, or
 * links.
 */
function anyValue(value: SampleLabel[1]): AnyValueInput {
  if (typeof value === 'string') return {stringValue: value};
  if (typeof value === 'number' && !Number.isInteger(value)) {
    return {doubleValue: value};
  }
  return {intValue: value};
}

function isAllZero(bytes: Uint8Array): boolean {
  return bytes.every(byte => byte === 0);
}

function hex(bytes: Uint8Array): string {
  return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.length).toString(
    'hex',
  );
}

/**
 * Serializes a V8 time profile as OpenTelemetry profiles: wall time, CPU
 * time if the profile has it, and a sample count unless it can be folded into
 * the wall time profile. Pass `samplePerContext` to give every sample with a
 * context a timestamp, which lets the count be folded.
 */
export function serializeTimeProfileOtel(
  prof: TimeProfile,
  options: TimeProfileNormalizerOptions & OtelSerializerOptions,
): ExportProfilesServiceRequest {
  const builder = new OtelProfilesBuilder(options);
  visitTimeProfile(prof, builder.visitor(), options);
  return builder.build();
}

/**
 * Serializes a V8 heap profile as OpenTelemetry profiles: in-use objects and
 * space and, in allocation mode, allocated objects and space.
 */
export function serializeHeapProfileOtel(
  prof: AllocationProfileNode | AllocationProfileNodeWithStats,
  options: HeapProfileNormalizerOptions & OtelSerializerOptions,
): ExportProfilesServiceRequest {
  const builder = new OtelProfilesBuilder(options);
  visitHeapProfile(prof, builder.visitor(), options);
  return builder.build();
}
