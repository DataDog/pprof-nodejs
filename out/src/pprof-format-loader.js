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
exports.loadPprofFormat = loadPprofFormat;
const path_1 = require("path");
const runtime_1 = require("./runtime");
let cachedModule;
function loadFromPackageRoot() {
    const packageJsonPath = require.resolve('pprof-format/package.json');
    const packageRoot = packageJsonPath.slice(0, packageJsonPath.length - 'package.json'.length);
    return require((0, path_1.join)(packageRoot, 'dist/commonjs/index.js'));
}
function loadPprofFormat() {
    if (cachedModule) {
        return cachedModule;
    }
    try {
        const loaded = require('pprof-format');
        cachedModule = loaded;
        return loaded;
    }
    catch (error) {
        if (runtime_1.runtime !== 'bun') {
            throw error;
        }
    }
    cachedModule = loadFromPackageRoot();
    return cachedModule;
}
//# sourceMappingURL=pprof-format-loader.js.map