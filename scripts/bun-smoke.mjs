#!/usr/bin/env bun

import * as pprof from '../out/src/index.js';
import {runSmoke} from './bun-smoke-runner.mjs';

console.log(JSON.stringify(await runSmoke(pprof)));
