/** Coalesce new dial input, keeping one submission in flight. Failure drops later queued input. */
export class DialQueue {
  private pending = 0;
  private timer: ReturnType<typeof setTimeout> | undefined;
  private running = false;
  constructor(
    private submit: (delta: number) => Promise<void>,
    private failed: (error: unknown) => void,
    private delay = 120,
  ) {}
  add(delta: number) {
    if (!Number.isSafeInteger(delta)) return;
    this.pending = Math.max(-100, Math.min(100, this.pending + delta));
    if (!this.running && !this.timer)
      this.timer = setTimeout(() => void this.flush(), this.delay);
  }
  cancel() {
    if (this.timer) clearTimeout(this.timer);
    this.timer = undefined;
    this.pending = 0;
  }
  private async flush() {
    this.timer = undefined;
    if (!this.pending || this.running) return;
    const delta = this.pending;
    this.pending = 0;
    this.running = true;
    try {
      await this.submit(delta);
    } catch (error) {
      this.cancel();
      this.failed(error);
    } finally {
      this.running = false;
      if (this.pending && !this.timer)
        this.timer = setTimeout(() => void this.flush(), this.delay);
    }
  }
}
