/**
 * Copyright 2026 Datadog
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

export type Runtime = 'node' | 'bun';

const RUNTIME_ENV_KEY = 'DATADOG_PPROF_RUNTIME';

function env(): NodeJS.ProcessEnv {
  return process.env;
}

type RuntimeDetectionInputs = {
  envOverride: string | undefined;
  bunVersion: string | undefined;
  bunGlobal: unknown;
};

function detectRuntimeFromEnv(): Runtime | undefined {
  const override = env()[RUNTIME_ENV_KEY];
  if (override === 'node' || override === 'bun') {
    return override;
  }
  return undefined;
}

function detectRuntime(): Runtime {
  const envRuntime = detectRuntimeFromEnv();
  if (envRuntime) {
    return envRuntime;
  }

  return detectRuntimeFromInputs({
    envOverride: undefined,
    bunVersion: process.versions.bun,
    bunGlobal: (globalThis as {Bun?: unknown}).Bun,
  });
}

function detectRuntimeFromInputs({
  envOverride,
  bunVersion,
  bunGlobal,
}: RuntimeDetectionInputs): Runtime {
  if (envOverride === 'node' || envOverride === 'bun') {
    return envOverride;
  }

  if (typeof bunVersion === 'string') {
    return 'bun';
  }

  if (typeof bunGlobal !== 'undefined') {
    return 'bun';
  }

  return 'node';
}

export const __testing = {
  detectRuntimeFromInputs,
};

export const runtime = detectRuntime();
