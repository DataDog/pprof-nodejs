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

// Reports how this process sees AsyncContextFrame, for test-async-context-frame
// to compare against how the flags reached it. Also reports execArgv, so a
// failure shows whether the flag was visible there at all.

import {isAsyncContextFrameActive} from '../src/async-context-frame';

process.send?.({
  active: isAsyncContextFrameActive(),
  execArgv: process.execArgv,
});
