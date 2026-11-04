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

'use strict';

import {heap} from '../src/index';
import {
  SAMPLE_INTERVAL_BYTES,
  checkInuseProfile,
  leak,
} from './oom-profile-check';

// Same shape as the allocation fixtures, without allocation mode: covers the
// branch that renders v8's per-size counts instead of the sample stats.
heap.start(SAMPLE_INTERVAL_BYTES, 64);
heap.monitorOutOfMemory(
  'auto',
  1,
  true,
  undefined,
  checkInuseProfile,
  heap.CallbackMode.Interrupt,
);

for (;;) {
  leak();
}
