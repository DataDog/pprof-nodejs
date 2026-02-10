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
type RuntimeDetectionInputs = {
    envOverride: string | undefined;
    bunVersion: string | undefined;
    bunGlobal: unknown;
};
declare function detectRuntimeFromInputs({ envOverride, bunVersion, bunGlobal, }: RuntimeDetectionInputs): Runtime;
export declare const __testing: {
    detectRuntimeFromInputs: typeof detectRuntimeFromInputs;
};
export declare const runtime: Runtime;
export {};
