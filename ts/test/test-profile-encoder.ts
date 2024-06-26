/**
 * Copyright 2019 Google Inc. All Rights Reserved.
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

import {promisify} from 'util';
import {gunzip as gunzipCallback, gunzipSync} from 'zlib';

import {Profile} from 'pprof-format';
import {encode, encodeSync} from '../src/profile-encoder';

import {decodedTimeProfile, timeProfile} from './profiles-for-tests';

const assert = require('assert');
const gunzip = promisify(gunzipCallback);

describe('profile-encoded', () => {
  describe('encode', () => {
    it('should encode profile such that the encoded profile can be decoded', async () => {
      const encoded = await encode(timeProfile);
      const unzipped = await gunzip(encoded);
      const decoded = Profile.decode(unzipped);
      assert.deepEqual(decoded, decodedTimeProfile);
    });
  });
  describe('encodeSync', () => {
    it('should encode profile such that the encoded profile can be decoded', () => {
      const encoded = encodeSync(timeProfile);
      const unzipped = gunzipSync(encoded);
      const decoded = Profile.decode(unzipped);
      assert.deepEqual(decoded, decodedTimeProfile);
    });
  });
});
