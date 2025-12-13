/**
 * Copyright 2017 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
import * as sinon from 'sinon';
import * as tmp from 'tmp';

import {
  NON_JS_THREADS_FUNCTION_NAME,
  serializeHeapProfile,
  serializeTimeProfile,
} from '../src/profile-serializer';
import {SourceMapper} from '../src/sourcemapper/sourcemapper';
import {Label, Profile} from 'pprof-format';
import {TimeProfile, TimeProfileNode} from '../src/v8-types';
import {
  anonymousFunctionHeapProfile,
  getAndVerifyPresence,
  getAndVerifyString,
  heapProfile,
  heapSourceProfile,
  labelEncodingProfile,
  mapDirPath,
  timeProfile,
  timeSourceProfile,
  v8AnonymousFunctionHeapProfile,
  v8HeapGeneratedProfile,
  v8HeapProfile,
  v8TimeGeneratedProfile,
  v8TimeProfile,
} from './profiles-for-tests';

const assert = require('assert');

function getNonJSThreadsSample(profile: Profile): Number[] | null {
  for (const sample of profile.sample!) {
    const locationId = sample.locationId[0];
    const location = getAndVerifyPresence(
      profile.location!,
      locationId as number
    );
    const functionId = location.line![0].functionId;
    const fn = getAndVerifyPresence(profile.function!, functionId as number);
    const fn_name = profile.stringTable.strings[fn.name as number];
    if (fn_name === NON_JS_THREADS_FUNCTION_NAME) {
      return sample.value as Number[];
    }
  }

  return null;
}

describe('profile-serializer', () => {
  let dateStub: sinon.SinonStub<[], number>;

  before(() => {
    dateStub = sinon.stub(Date, 'now').returns(0);
  });
  after(() => {
    dateStub.restore();
  });

  describe('serializeTimeProfile', () => {
    it('should produce expected profile', () => {
      const timeProfileOut = serializeTimeProfile(v8TimeProfile, 1000);
      assert.deepEqual(timeProfileOut, timeProfile);
    });

    it('should omit non-jS threads CPU time when profile has no CPU time', () => {
      const timeProfile: TimeProfile = {
        startTime: 0,
        endTime: 10 * 1000 * 1000,
        hasCpuTime: false,
        nonJSThreadsCpuTime: 1000,
        topDownRoot: {
          name: '(root)',
          scriptName: 'root',
          scriptId: 0,
          lineNumber: 0,
          columnNumber: 0,
          hitCount: 0,
          children: [],
        },
      };
      const timeProfileOut = serializeTimeProfile(timeProfile, 1000);
      assert.equal(getNonJSThreadsSample(timeProfileOut), null);
      const timeProfileOutWithLabels = serializeTimeProfile(
        timeProfile,
        1000,
        undefined,
        false,
        () => {
          return {foo: 'bar'};
        }
      );
      assert.equal(getNonJSThreadsSample(timeProfileOutWithLabels), null);
    });

    it('should omit non-jS threads CPU time when it is zero', () => {
      const timeProfile: TimeProfile = {
        startTime: 0,
        endTime: 10 * 1000 * 1000,
        hasCpuTime: true,
        nonJSThreadsCpuTime: 0,
        topDownRoot: {
          name: '(root)',
          scriptName: 'root',
          scriptId: 0,
          lineNumber: 0,
          columnNumber: 0,
          hitCount: 0,
          children: [],
        },
      };
      const timeProfileOut = serializeTimeProfile(timeProfile, 1000);
      assert.equal(getNonJSThreadsSample(timeProfileOut), null);
      const timeProfileOutWithLabels = serializeTimeProfile(
        timeProfile,
        1000,
        undefined,
        false,
        () => {
          return {foo: 'bar'};
        }
      );
      assert.equal(getNonJSThreadsSample(timeProfileOutWithLabels), null);
    });

    it('should produce Non-JS thread sample with zero wall time', () => {
      const timeProfile: TimeProfile = {
        startTime: 0,
        endTime: 10 * 1000 * 1000,
        hasCpuTime: true,
        nonJSThreadsCpuTime: 1000,
        topDownRoot: {
          name: '(root)',
          scriptName: 'root',
          scriptId: 0,
          lineNumber: 0,
          columnNumber: 0,
          hitCount: 0,
          children: [],
        },
      };
      const timeProfileOut = serializeTimeProfile(timeProfile, 1000);
      const values = getNonJSThreadsSample(timeProfileOut);
      assert.notEqual(values, null);
      assert.equal(values![0], 0);
      assert.equal(values![1], 0);
      assert.equal(values![2], 1000);
      const timeProfileOutWithLabels = serializeTimeProfile(
        timeProfile,
        1000,
        undefined,
        false,
        () => {
          return {foo: 'bar'};
        }
      );
      const valuesWithLabels = getNonJSThreadsSample(timeProfileOutWithLabels);
      assert.notEqual(valuesWithLabels, null);
      assert.equal(valuesWithLabels![0], 0);
      assert.equal(valuesWithLabels![1], 0);
      assert.equal(valuesWithLabels![2], 1000);
    });
  });

  describe('label builder', () => {
    it('should accept strings, numbers, and bigints', () => {
      const profileOut = serializeTimeProfile(labelEncodingProfile, 1000);
      const st = profileOut.stringTable;
      assert.deepEqual(profileOut.sample[0].label, [
        new Label({key: st.dedup('someStr'), str: st.dedup('foo')}),
        new Label({key: st.dedup('someNum'), num: 42}),
        new Label({key: st.dedup('someBigint'), num: 18446744073709551557n}),
      ]);
    });
  });

  describe('serializeHeapProfile', () => {
    it('should produce expected profile', () => {
      const heapProfileOut = serializeHeapProfile(v8HeapProfile, 0, 512 * 1024);
      assert.deepEqual(heapProfileOut, heapProfile);
    });
    it('should produce expected profile when there is anonymous function', () => {
      const heapProfileOut = serializeHeapProfile(
        v8AnonymousFunctionHeapProfile,
        0,
        512 * 1024
      );
      assert.deepEqual(heapProfileOut, anonymousFunctionHeapProfile);
    });
  });

  describe('source map specified', () => {
    let sourceMapper: SourceMapper;
    before(async () => {
      const sourceMapFiles = [mapDirPath];
      sourceMapper = await SourceMapper.create(sourceMapFiles);
    });

    describe('serializeHeapProfile', () => {
      it('should produce expected profile', () => {
        const heapProfileOut = serializeHeapProfile(
          v8HeapGeneratedProfile,
          0,
          512 * 1024,
          undefined,
          sourceMapper
        );
        assert.deepEqual(heapProfileOut, heapSourceProfile);
      });
    });

    describe('serializeTimeProfile', () => {
      it('should produce expected profile', () => {
        const timeProfileOut = serializeTimeProfile(
          v8TimeGeneratedProfile,
          1000,
          sourceMapper
        );
        assert.deepEqual(timeProfileOut, timeSourceProfile);
      });
    });

    after(() => {
      tmp.setGracefulCleanup();
    });
  });

  describe('source map with column 0 (LineTick simulation)', () => {
    // This tests the LEAST_UPPER_BOUND fallback for when V8's LineTick
    // doesn't provide column information (column=0)
    let sourceMapper: SourceMapper;
    let testMapDir: string;

    // Line in source.ts that the first call maps to (column 10)
    const FIRST_CALL_SOURCE_LINE = 100;
    // Line in source.ts that the second call maps to (column 25)
    const SECOND_CALL_SOURCE_LINE = 200;

    before(async () => {
      // Create a source map simulating: return fib(n-1) + fib(n-2)
      // Same function called twice on the same line at different columns
      testMapDir = tmp.dirSync().name;
      const {SourceMapGenerator} = await import('source-map');
      const fs = await import('fs');
      const path = await import('path');

      const mapGen = new SourceMapGenerator({file: 'generated.js'});

      // First fib() call at column 10 -> maps to source line 100
      mapGen.addMapping({
        source: path.join(testMapDir, 'source.ts'),
        name: 'fib',
        generated: {line: 5, column: 10},
        original: {line: FIRST_CALL_SOURCE_LINE, column: 0},
      });

      // Second fib() call at column 25 -> maps to source line 200
      mapGen.addMapping({
        source: path.join(testMapDir, 'source.ts'),
        name: 'fib',
        generated: {line: 5, column: 25},
        original: {line: SECOND_CALL_SOURCE_LINE, column: 0},
      });

      fs.writeFileSync(
        path.join(testMapDir, 'generated.js.map'),
        mapGen.toString()
      );
      fs.writeFileSync(path.join(testMapDir, 'generated.js'), '');

      sourceMapper = await SourceMapper.create([testMapDir]);
    });

    it('should map column 0 to first mapping on line (LEAST_UPPER_BOUND fallback)', () => {
      const path = require('path');
      // Simulate LineTick entry with column=0 (no column info from V8 < 14)
      // This is the fallback behavior when LineTick.column is not available
      const childNode: TimeProfileNode = {
        name: 'fib',
        scriptName: path.join(testMapDir, 'generated.js'),
        scriptId: 1,
        lineNumber: 5,
        columnNumber: 0, // LineTick has no column in V8 < 14
        hitCount: 1,
        children: [],
      };
      const v8Profile: TimeProfile = {
        startTime: 0,
        endTime: 1000000,
        topDownRoot: {
          name: '(root)',
          scriptName: 'root',
          scriptId: 0,
          lineNumber: 0,
          columnNumber: 0,
          hitCount: 0,
          children: [childNode],
        },
      };

      const profile = serializeTimeProfile(v8Profile, 1000, sourceMapper);

      assert.strictEqual(profile.location!.length, 1);
      const loc = profile.location![0];
      const line = loc.line![0];
      const func = getAndVerifyPresence(
        profile.function!,
        line.functionId as number
      );
      const filename = getAndVerifyString(
        profile.stringTable,
        func,
        'filename'
      );

      // Should be mapped to source.ts
      assert.ok(
        filename.includes('source.ts'),
        `Expected source.ts but got ${filename}`
      );
      // With column 0 and LEAST_UPPER_BOUND, should map to FIRST mapping (line 100)
      assert.strictEqual(
        line.line,
        FIRST_CALL_SOURCE_LINE,
        'Column 0 should use LEAST_UPPER_BOUND to find first mapping on line'
      );
    });

    it('should map to second call when column points to it (V8 14+ with LineTick.column)', () => {
      const path = require('path');
      // Simulate V8 14+ behavior where LineTick has actual column data
      // Column 26 is after the second mapping at column 25
      const childNode: TimeProfileNode = {
        name: 'fib',
        scriptName: path.join(testMapDir, 'generated.js'),
        scriptId: 1,
        lineNumber: 5,
        columnNumber: 26, // V8 14+ provides actual column from LineTick
        hitCount: 1,
        children: [],
      };
      const v8Profile: TimeProfile = {
        startTime: 0,
        endTime: 1000000,
        topDownRoot: {
          name: '(root)',
          scriptName: 'root',
          scriptId: 0,
          lineNumber: 0,
          columnNumber: 0,
          hitCount: 0,
          children: [childNode],
        },
      };

      const profile = serializeTimeProfile(v8Profile, 1000, sourceMapper);

      assert.strictEqual(profile.location!.length, 1);
      const loc = profile.location![0];
      const line = loc.line![0];

      // Column 26 with GREATEST_LOWER_BOUND should map to second call (line 200)
      assert.strictEqual(
        line.line,
        SECOND_CALL_SOURCE_LINE,
        'Column 26 should use GREATEST_LOWER_BOUND to find mapping at column 25'
      );
    });

    it('should map to first call when column points to it (V8 14+ with LineTick.column)', () => {
      const path = require('path');
      // Simulate V8 14+ behavior where LineTick has actual column data
      // Column 11 is after the first mapping at column 10 but before second at 25
      const childNode: TimeProfileNode = {
        name: 'fib',
        scriptName: path.join(testMapDir, 'generated.js'),
        scriptId: 1,
        lineNumber: 5,
        columnNumber: 11, // V8 14+ provides actual column from LineTick
        hitCount: 1,
        children: [],
      };
      const v8Profile: TimeProfile = {
        startTime: 0,
        endTime: 1000000,
        topDownRoot: {
          name: '(root)',
          scriptName: 'root',
          scriptId: 0,
          lineNumber: 0,
          columnNumber: 0,
          hitCount: 0,
          children: [childNode],
        },
      };

      const profile = serializeTimeProfile(v8Profile, 1000, sourceMapper);

      assert.strictEqual(profile.location!.length, 1);
      const loc = profile.location![0];
      const line = loc.line![0];

      // Column 11 with GREATEST_LOWER_BOUND should map to first call (line 100)
      assert.strictEqual(
        line.line,
        FIRST_CALL_SOURCE_LINE,
        'Column 11 should use GREATEST_LOWER_BOUND to find mapping at column 10'
      );
    });
  });
});
