import React from 'react';
import {Box, Text} from 'ink';
import {artifactKeys, artifactLabels, display, stagesFor, type Draft, type PlanSummary, type Progress} from './model.js';
import type {Light} from './discovery.js';

export const palette = {ink: '#EDE9DF', muted: '#8B9798', accent: '#A4E6D5', line: '#405158', amber: '#F1C477', red: '#F1948A'};
export function Key({children}: {children: React.ReactNode}) {
  return <Text color={palette.accent}>{children}</Text>;
}
export function Frame({children, width = 96, mode, footer, compact = width <= 80}: {children: React.ReactNode; width?: number; mode?: string; footer?: string; compact?: boolean}) {
  return <Box flexDirection="column" width={Math.max(36, Math.min(width, 104))} paddingX={1}>
    <Box marginTop={compact ? 0 : 1} marginBottom={mode ? 0 : 1} justifyContent="space-between"><Text bold color={palette.ink}>◉  OPEN KEYLIGHT <Text color={palette.accent}>CHROMA</Text></Text><Text color={palette.muted}>INSTALLER</Text></Box>
    {mode && <Box marginTop={1} marginBottom={1}><Text color={palette.muted}>{mode}</Text></Box>}
    <Box flexDirection="column" borderStyle="round" borderColor={palette.line} paddingX={2} paddingY={compact ? 0 : 1}>{children}</Box>
    <Box marginTop={1} marginBottom={compact ? 0 : 1}><Text color={palette.muted}>{footer ?? 'Tab / ↑↓ move   Enter continue   Esc back   Ctrl+C exit'}</Text></Box>
  </Box>;
}
export function Heading({eyebrow, title, detail}: {eyebrow: string; title: string; detail?: string}) {
  return <Box flexDirection="column" marginBottom={1}>
    <Text color={palette.accent}>{eyebrow}</Text>
    <Text bold color={palette.ink}>{display(title)}</Text>
    {detail && <Text color={palette.muted}>{display(detail, 300)}</Text>}
  </Box>;
}
export function Field({label, value, active, cursor = value.length, hint}: {label: string; value: string; active: boolean; cursor?: number; hint?: string}) {
  const characters = Array.from(display(value, 1000)), start = Math.max(0, cursor - 30);
  const before = characters.slice(start, cursor).join(''), after = characters.slice(cursor, cursor + 18);
  const clean = characters.length > 54 ? characters.slice(0, 20).join('') + '…' + characters.slice(-30).join('') : characters.join('');
  return <Box flexDirection="column" marginBottom={1}>
    <Text color={active ? palette.accent : palette.muted}>{active ? '› ' : '  '}{label}</Text>
    <Text color={palette.ink}>  {active && start ? '…' : ''}{active ? <>{before}<Text inverse>{after[0] ?? ' '}</Text>{after.slice(1).join('')}</> : clean || <Text color={palette.line}>Not selected</Text>}</Text>
    {active && hint && <Text color={palette.muted}>  {hint}</Text>}
  </Box>;
}
export function Button({label, active = true, compact = false}: {label: string; active?: boolean; compact?: boolean}) {
  return <Box marginTop={compact ? 0 : 1}><Text bold color={active ? palette.accent : palette.muted}>{active ? '› ' : '  '}{display(label)}</Text></Box>;
}
export function LightChoices({lights, selected}: {lights: Light[]; selected: number}) {
  const first = Math.max(0, Math.min(selected - 1, lights.length - 3));
  return <>
    <Heading eyebrow="FIND YOUR LIGHT" title="Choose a light." />
    {lights.length === 0 && <Box flexDirection="column" marginBottom={1}>
      <Text color={palette.muted}>Keep your light powered and use the same local network.</Text>
    </Box>}
    {lights.slice(first, first + 3).map((light, offset) => <Box key={`${light.ip}/${light.deviceId}`} flexDirection="column" marginBottom={1}><Button compact label={`${light.name}${light.installed ? ' · Open Keylight' : ''}`} active={selected === first + offset} /><Text color={palette.muted}>  {light.ip}</Text></Box>)}
    {lights.length > 3 && <Text color={palette.muted}>{first + 1}–{Math.min(first + 3, lights.length)} of {lights.length} · ↑↓ browse</Text>}
    <Button compact label="Look again" active={selected === lights.length} />
    <Button compact label="Enter stock address manually" active={selected === lights.length + 1} />
  </>;
}
export function Notice({children, error = false}: {children: string; error?: boolean}) {
  return <Box marginTop={1}><Text color={error ? palette.red : palette.amber}>{display(children, 500)}</Text></Box>;
}
export function ArtifactList({draft}: {draft: Draft}) {
  return <Box flexDirection="column" marginY={1}>
    {artifactKeys.map(key => <Box key={key}><Box width={29}><Text color={draft[key] ? palette.ink : palette.amber}>{draft[key] ? '○' : '–'} {artifactLabels[key]}</Text></Box><Text color={palette.muted} wrap="truncate-middle">{draft[key] ? display(draft[key]) : 'Choose a file'}</Text></Box>)}
  </Box>;
}
export function PlanCard({summary, details = false}: {summary: PlanSummary; details?: boolean}) {
  return <Box flexDirection="column">
    <Heading eyebrow="READY TO REVIEW" title={display(summary.target_name)} detail={summary.target_ip} />
    <Text color={palette.ink}>Open Keylight {summary.esp.version}</Text>
    <Box marginY={1}><Text color={palette.accent}>✓ Firmware and recovery files ready</Text></Box>
    <Text color={palette.muted}>Watch for darkness, then five gentle colour pulses.</Text>
    {details && <Box marginTop={1} flexDirection="column"><Text color={palette.muted}>{summary.device_id} · plan {summary.manifest_sha256}</Text><Text color={palette.muted}>{display(summary.restore_provenance, 1000)}</Text><Text color={palette.muted}>Files checked. The light has not been contacted yet.</Text></Box>}
  </Box>;
}
export function ProgressView({progress, preview = false, width = 96, yes = false, running = false, auditPath, details = false, targetName}: {progress: Progress; preview?: boolean; width?: number; yes?: boolean; running?: boolean; auditPath?: string; details?: boolean; targetName?: string}) {
  const stages = stagesFor(progress.workflow);
  const count = progress.completed ?? 0, total = progress.total ?? 0;
  const stopped = progress.state === 'stopped';
  const timed = progress.state === 'quiet' || progress.unit === 'seconds';
  const validCount = !stopped && !timed && total > 0 && count >= 0 && count <= total;
  const bars = 24, filled = validCount ? Math.floor(count / total * bars) : 0;
  const footer = preview ? 'Illustrative progress · Esc back · Ctrl+C exit' : stopped && running ? 'D details · Waiting for a safe stop' : progress.prompt && !stopped ? '↑↓ choose · Enter confirm · Ctrl+C stop' : running ? 'D details · Ctrl+C stop safely' : 'Esc back · D details · Ctrl+C exit';
  const headline = stopped ? running ? 'Stopping safely' : 'Installation stopped' : progress.state === 'complete' ? 'Ready to use' : stages[progress.stage]?.[1] ?? 'Installing';
  return <Frame width={width} mode={preview ? 'DEMO · NO DEVICE ACTIVITY' : targetName ?? (progress.workflow === 'finish' ? 'FINISH YOUR INSTALLATION' : 'INSTALLING')} footer={footer}>
    <Box flexDirection={width < 76 ? 'column' : 'row'} gap={2}>
      <Box flexDirection="column" width={width < 76 ? undefined : 29} flexShrink={0}>
        {stages.map(([id, title], i) => <Box key={id}><Text color={i === progress.stage ? palette.accent : palette.muted}>{progress.finishedStages?.includes(i) ? '✓' : String(i + 1).padStart(2, '0')}  {title}</Text></Box>)}
      </Box>
      <Box flexDirection="column" flexGrow={1}>
        <Heading eyebrow={`STEP ${progress.stage + 1} / ${stages.length}`} title={headline} />
        {stopped ? <Box flexDirection="column"><Text color={palette.amber}>{/timed? ?out|deadline|Peer closed|ConnectionReset/i.test(progress.label) ? 'The light stopped responding.' : 'A setup check did not pass.'}</Text><Text color={palette.muted}>{running ? 'Keep this window open and the light powered.' : progress.stage === 0 && progress.workflow !== 'finish' && progress.failureCode === 'stock_loader_entry_unconfirmed' ? 'No firmware was uploaded.' : 'Open details before continuing.'}</Text></Box> :
          timed && !progress.action ? <Text color={palette.ink}>{progress.state === 'quiet' ? 'Waiting for the controller to restart…' : progress.stage === stages.length - 1 ? 'Finishing setup…' : 'Waiting for the dashboard…'}</Text> :
          !progress.prompt && !progress.action && progress.state !== 'complete' && <Text color={palette.ink}>{details || validCount ? display(progress.label) : 'Checking and preparing this step…'}</Text>}
        {validCount && <Box marginTop={1} flexDirection="column"><Text color={palette.accent}>{'━'.repeat(filled)}<Text color={palette.line}>{'─'.repeat(bars - filled)}</Text></Text><Text color={palette.muted}>{Math.floor(count / total * 100)}%{details || preview ? ` · ${count} / ${total}` : ''}</Text></Box>}
        {timed && !stopped && <Text color={palette.muted}>Keep the light powered.</Text>}
        {!stopped && progress.prompt && <Box flexDirection="column"><Text bold color={palette.amber}>{display(progress.prompt.question)}</Text><Button label="No / unsure" active={!yes} /><Button label="Yes" active={yes} /></Box>}
        {!stopped && progress.action && <Box marginTop={1} flexDirection="column"><Text bold color={palette.accent}>Your new dashboard is ready</Text><Text>{progress.action.url}</Text><Text color={palette.muted}>{progress.acceptanceState === 'complete' ? 'Connection saved. Finishing checks…' : progress.acceptanceState === 'running' || progress.action.pairingOpen ? 'Connecting and checking the light…' : 'Hold the light’s button for 3 seconds to pair.'}</Text><Text color={palette.amber}>{progress.remainingSeconds ?? progress.action.remainingSeconds}s left to finish setup</Text></Box>}
        {progress.state === 'complete' && progress.dashboardUrl && <Box marginTop={1} flexDirection="column"><Text bold color={palette.accent}>Your light is ready.</Text><Text color={palette.ink}>{progress.dashboardUrl}</Text><Text color={palette.muted}>Open your dashboard for colours, scenes and settings.</Text></Box>}
        {!stopped && progress.credentialPath && <Box marginTop={1} flexDirection="column"><Text color={palette.accent}>Connection saved on this computer.</Text>{details && <Text color={palette.muted} wrap="truncate-middle">{display(progress.credentialPath, 1000)}</Text>}</Box>}
        {!stopped && progress.cancelRequested && <Notice>Stopping after this step. Keep power connected.</Notice>}
        {details && <Box marginTop={1} flexDirection="column"><Text color={palette.muted}>{display(progress.label, 1000)}</Text>{auditPath && <Text color={palette.muted} wrap="truncate-middle">Audit: {display(auditPath, 1000)}</Text>}</Box>}
      </Box>
    </Box>
  </Frame>;
}
