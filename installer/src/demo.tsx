import React from 'react';
import {Box, Text} from 'ink';
import {Frame, Button, LightChoices, Notice, PlanCard, ProgressView, palette} from './components.js';
import type {PlanSummary, Progress} from './model.js';

export const demoScenes = ['discover', 'review', 'install', 'observe', 'complete'] as const;
export type DemoScene = typeof demoScenes[number];
const summary: PlanSummary = {profile: 'keylight-chroma-1.0.13', target_ip: '192.168.1.25', target_name: 'Studio light', device_id: 'keylight-aabbcc', manifest_sha256: 'a'.repeat(64), packages: {identity: '1'.repeat(64), OFF1: '2'.repeat(64), LOW1: '3'.repeat(64), lighting: '4'.repeat(64)}, restore_sha256: '5'.repeat(64), restore_provenance: 'Demonstration fixture. No firmware files or device evidence.', esp: {version: '0.2.0', sha256: '6'.repeat(64), elf_sha256: '7'.repeat(64), bytes: 1200000}, controller_version: '0.1.1.0', device_operations: 0};

/** Fixed presentation fixtures. No effects, timers, file access or capabilities. */
export function Demo({scene, width = 96}: {scene: DemoScene; width?: number}) {
  if (scene === 'discover' || scene === 'review') return <Frame width={width} mode="DEMO · NO DEVICE ACTIVITY" footer="Illustrative interface · no discovery, downloads or installation">
    {scene === 'discover' ? <>
      <LightChoices selected={0} lights={[{name: 'Studio light', ip: '192.168.1.25', deviceId: 'keylight-aabbcc', mac: 'AA:BB:CC:AA:BB:CC', installed: false}, {name: 'Desk light', ip: '192.168.1.26', deviceId: 'keylight-ddeeff', installed: true}]} />
      <Notice>Demonstration devices. No network query has been made.</Notice>
    </> : <><PlanCard summary={summary} /><Button label="Review installation and start" /><Text color={palette.muted}>D show technical details</Text></>}
  </Frame>;
  const progress: Progress = scene === 'install'
    ? {stage: 4, state: 'running', label: 'Reading back every byte', completed: 336, total: 448, finishedStages: [0, 1, 2, 3]}
    : scene === 'observe'
      ? {stage: 3, state: 'prompt', label: 'The controller has returned safely.', finishedStages: [0, 1, 2], prompt: {id: 'demo-only', question: 'Did you see red, green, blue, warm and cool — with darkness between?'}}
      : {stage: 6, state: 'complete', label: 'Installation verified and dashboard confirmed.', finishedStages: [0, 1, 2, 3, 4, 5, 6], dashboardUrl: 'http://192.168.1.25/', credentialPath: '~/.open-keylight/credentials/demo/access.json'};
  return <ProgressView progress={progress} preview width={width} />;
}
