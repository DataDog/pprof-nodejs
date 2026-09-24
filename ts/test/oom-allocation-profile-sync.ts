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
import {SAMPLE_INTERVAL_BYTES, leak} from './oom-profile-check';

// Never yields, so a deferred capture would dump nothing. No callback on
// purpose: delivering one needs heap room this process no longer has.
heap.start(SAMPLE_INTERVAL_BYTES, 64, true);
heap.monitorOutOfMemory('auto', 1, true);

for (;;) {
  leak();
}
