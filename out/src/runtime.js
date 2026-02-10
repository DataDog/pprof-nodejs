"use strict";
/**
 * Copyright 2026 Datadog
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.runtime = exports.__testing = void 0;
const RUNTIME_ENV_KEY = 'DATADOG_PPROF_RUNTIME';
function env() {
    return process.env;
}
function detectRuntimeFromEnv() {
    const override = env()[RUNTIME_ENV_KEY];
    if (override === 'node' || override === 'bun') {
        return override;
    }
    return undefined;
}
function detectRuntime() {
    const envRuntime = detectRuntimeFromEnv();
    if (envRuntime) {
        return envRuntime;
    }
    return detectRuntimeFromInputs({
        envOverride: undefined,
        bunVersion: process.versions.bun,
        bunGlobal: globalThis.Bun,
    });
}
function detectRuntimeFromInputs({ envOverride, bunVersion, bunGlobal, }) {
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
exports.__testing = {
    detectRuntimeFromInputs,
};
exports.runtime = detectRuntime();
//# sourceMappingURL=runtime.js.map