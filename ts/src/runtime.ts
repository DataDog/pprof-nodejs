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

function detectRuntimeFromEnv(): Runtime | undefined {
  const override = env()[RUNTIME_ENV_KEY];
  if (override === 'node' || override === 'bun') {
    return override;
  }
  return undefined;
}

function detectRuntime(): Runtime {
  const overriddenRuntime = detectRuntimeFromEnv();
  if (overriddenRuntime) {
    return overriddenRuntime;
  }

  if (typeof process.versions.bun === 'string') {
    return 'bun';
  }

  return 'node';
}

export const runtime = detectRuntime();
