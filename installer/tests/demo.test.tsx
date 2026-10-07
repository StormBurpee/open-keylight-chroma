import React from 'react';
import {test, mock} from 'node:test';
import assert from 'node:assert/strict';
import {renderToString} from 'ink';
import childProcess from 'node:child_process';
import dgram from 'node:dgram';
import http from 'node:http';
import https from 'node:https';
import {Demo, demoScenes} from '../src/demo.js';

test('all demo scenes use real Ink rendering deterministically, with no network or subprocess', () => {
  const forbidden = () => {throw new Error('Demo attempted external activity');};
  const guards = [mock.method(childProcess, 'spawn', forbidden), mock.method(childProcess, 'execFile', forbidden), mock.method(dgram, 'createSocket', forbidden), mock.method(http, 'request', forbidden), mock.method(https, 'request', forbidden), mock.method(globalThis, 'fetch', forbidden)];
  try {
    for (const scene of demoScenes) for (const columns of [60, 80, 96, 120]) {
      const render = () => renderToString(<Demo scene={scene} width={columns} />, {columns});
      assert.equal(render(), render()); assert.match(render(), /NO DEVICE\s+ACTIVITY/);
      assert.ok(render().split('\n').every(line => Array.from(line).length <= columns));
    }
    assert.match(renderToString(<Demo scene="observe" />), /No \/ unsure/);
    assert.match(renderToString(<Demo scene="complete" />), /192\.168\.1\.25/);
    assert.ok(renderToString(<Demo scene="discover" width={80} />, {columns: 80}).trim().split('\n').length <= 24, 'Discovery must fit 80×24');
    for (const guard of guards) assert.equal(guard.mock.callCount(), 0);
  } finally {guards.forEach(guard => guard.mock.restore());}
});
