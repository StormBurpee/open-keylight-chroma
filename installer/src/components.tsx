import React from 'react';
import {Box, Text} from 'ink';
import {artifactKeys, artifactLabels, display, stages, type Draft, type PlanSummary, type Progress} from './model.js';
import type {Light} from './discovery.js';

export const palette = {ink: '#EDE9DF', muted: '#8B9798', accent: '#A4E6D5', line: '#405158', amber: '#F1C477', red: '#F1948A'};
export function Key({children}: {children: React.ReactNode}) {
  return <Text color={palette.accent}>{children}</Text>;
}
export function Frame({children, width = 96, mode = 'LOCAL PREPARATION', footer, compact = width <= 80}: {children: React.ReactNode; width?: number; mode?: string; footer?: string; compact?: boolean}) {
  return <Box flexDirection="column" width={Math.max(36, Math.min(width, 104))} paddingX={1}>
    <Box marginTop={compact ? 0 : 1} justifyContent="space-between"><Text bold color={palette.ink}>◉  OPEN KEYLIGHT <Text color={palette.accent}>CHROMA</Text></Text><Text color={palette.muted}>INSTALLER</Text></Box>
    <Box marginTop={1} marginBottom={1}><Text color={palette.muted}>Your light. Your firmware. <Text color={palette.amber}>{mode}</Text></Text></Box>
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
    <Heading eyebrow="01 / FIND YOUR LIGHT" title="Which light are we making yours?" detail="Find lights on your local network. Nothing is changed." />
    {lights.length === 0 && <Text color={palette.muted}>Keep the light and computer on the same network. Look again, or use Advanced with its router address.</Text>}
    {lights.slice(first, first + 3).map((light, offset) => <Box key={`${light.ip}/${light.deviceId}`} flexDirection="column" marginBottom={1}><Button compact label={`${light.name}${light.installed ? ' · already installed' : ''}`} active={selected === first + offset} /><Text color={palette.muted}>  {light.ip} · {light.mac ?? light.deviceId}</Text></Box>)}
    {lights.length > 3 && <Text color={palette.muted}>{first + 1}–{Math.min(first + 3, lights.length)} of {lights.length} · ↑↓ browse</Text>}
    <Button compact label="Look again" active={selected === lights.length} />
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
    <Box marginY={1} flexDirection="column"><Text color={palette.accent}>✓ Firmware for both chips + your local dashboard</Text><Text color={palette.accent}>✓ Verified recovery image available</Text></Box>
    <Text color={palette.ink}>Two quick visual checks, with you watching the light.</Text>
    <Text color={palette.muted}>First darkness, then five gentle colour pulses.</Text>
    {details && <Box marginTop={1} flexDirection="column"><Text color={palette.muted}>{summary.device_id} · plan {summary.manifest_sha256}</Text><Text color={palette.muted}>{display(summary.restore_provenance, 1000)}</Text></Box>}
    <Notice>Files checked. The light has not been contacted yet.</Notice>
  </Box>;
}
export function ProgressView({progress, preview = false, width = 96, yes = false, running = false}: {progress: Progress; preview?: boolean; width?: number; yes?: boolean; running?: boolean}) {
  const count = progress.completed ?? 0, total = progress.total ?? 0;
  const validCount = total > 0 && count >= 0 && count <= total;
  const bars = 24, filled = validCount ? Math.floor(count / total * bars) : 0;
  return <Frame width={width} mode={preview ? 'INTERFACE PREVIEW · NO DEVICE ACTIVITY' : 'GUIDED INSTALLATION'} footer={preview ? 'Esc back   Ctrl+C exit' : running ? '↑↓ choose observation   Enter answer   Ctrl+C stop after this stage' : 'Esc back   Ctrl+C exit'}>
    <Box flexDirection={width < 76 ? 'column' : 'row'} gap={2}>
      <Box flexDirection="column" width={width < 76 ? undefined : 29} flexShrink={0}>
        {stages.map(([id, title], i) => <Box key={id}><Text color={i === progress.stage ? palette.accent : palette.muted}>{progress.finishedStages?.includes(i) ? '✓' : String(i + 1).padStart(2, '0')}  {title}</Text></Box>)}
      </Box>
      <Box flexDirection="column" flexGrow={1}>
        <Heading eyebrow={`STEP ${progress.stage + 1} / 7`} title={stages[progress.stage]?.[1] ?? 'Installer status'} detail={stages[progress.stage]?.[2]} />
        <Text color={palette.ink}>{display(progress.label)}</Text>
        {validCount && <Box marginTop={1} flexDirection="column"><Text color={palette.accent}>{'━'.repeat(filled)}<Text color={palette.line}>{'─'.repeat(bars - filled)}</Text></Text><Text color={palette.muted}>{count} / {total} · {Math.floor(count / total * 100)}%</Text></Box>}
        {progress.state === 'quiet' && <Notice>{`Letting the controller finish and restart${progress.remainingSeconds === undefined ? '' : ` · ${progress.remainingSeconds}s remaining`}. Keep the light powered.`}</Notice>}
        {progress.prompt && <Box marginTop={1} flexDirection="column"><Text bold color={palette.amber}>{display(progress.prompt.question)}</Text><Text color={palette.muted}>Choose only what you observed. No answer is assumed.</Text><Button label="No / unsure — stop" active={!yes} /><Button label="Yes — the observation matches" active={yes} /></Box>}
        {progress.action && <Box marginTop={1} flexDirection="column"><Text bold color={palette.accent}>Your new dashboard is ready</Text><Text>{progress.action.url}</Text><Text color={palette.muted}>{progress.acceptanceState === 'complete' ? 'Access saved and application confirmed. Finishing independent checks.' : progress.acceptanceState === 'running' || progress.action.pairingOpen ? 'Verifying pairing, a brief 5% white check, Off and application acceptance.' : 'Hold the light’s button for 3 seconds to open pairing. Waiting for a fresh device check.'}</Text><Text color={palette.amber}>Trial: {progress.remainingSeconds ?? progress.action.remainingSeconds}s remaining at last check</Text></Box>}
        {progress.state === 'complete' && progress.dashboardUrl && <Box marginTop={1} flexDirection="column"><Text bold color={palette.accent}>Your light is ready.</Text><Text color={palette.ink}>{progress.dashboardUrl}</Text><Text color={palette.muted}>Open your dashboard for colours, scenes and settings.</Text></Box>}
        {progress.credentialPath && <Box marginTop={1} flexDirection="column"><Text color={palette.accent}>Your access token is saved privately</Text><Text color={palette.muted} wrap="truncate-middle">{display(progress.credentialPath, 1000)}</Text><Text color={palette.muted}>Use it in dashboard Connect access or Stream Deck.</Text></Box>}
        {progress.cancelRequested && <Notice>Stop requested. Finishing the current safe stage; keep power and this window open.</Notice>}
        {preview && <Notice>Illustrative progress only. These values do not describe a connected light.</Notice>}
      </Box>
    </Box>
  </Frame>;
}
