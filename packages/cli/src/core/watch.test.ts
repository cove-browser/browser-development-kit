import { describe, expect, it, vi } from "vitest";
import { fakeWatch } from "../test-support.js";
import { watchLoop } from "./watch.js";

describe("watchLoop", () => {
  it("watches the directory, rebuilds on a change and stops watching when stopped", async () => {
    const fake = fakeWatch();
    const rebuild = vi.fn(() => Promise.resolve());
    const loop = watchLoop({ watch: fake.watch, directory: "/d", rebuild });
    expect(fake.watched).toEqual(["/d"]);
    expect(rebuild).not.toHaveBeenCalled();

    fake.change();
    await vi.waitFor(() => expect(rebuild).toHaveBeenCalledTimes(1));
    fake.stop();
    await loop;
    expect(fake.watched).toEqual([]);
  });

  it("makes one rebuild of a burst of changes", async () => {
    const fake = fakeWatch();
    const rebuild = vi.fn(() => Promise.resolve());
    const loop = watchLoop({ watch: { ...fake.watch, debounceMs: 20 }, directory: "/d", rebuild });
    fake.change();
    fake.change();
    fake.change();
    await vi.waitFor(() => expect(rebuild).toHaveBeenCalledTimes(1));
    fake.stop();
    await loop;
    expect(rebuild).toHaveBeenCalledTimes(1);
  });

  it("runs another rebuild for a change during a rebuild, never two at once", async () => {
    const fake = fakeWatch();
    let active = 0;
    let overlapped = false;
    let release: () => void = () => {};
    const rebuild = vi.fn(async () => {
      active += 1;
      overlapped ||= active > 1;
      if (rebuild.mock.calls.length === 1) {
        await new Promise<void>((resolve) => {
          release = resolve;
        });
      }
      active -= 1;
    });
    const loop = watchLoop({ watch: fake.watch, directory: "/d", rebuild });
    fake.change();
    await vi.waitFor(() => expect(rebuild).toHaveBeenCalledTimes(1));
    fake.change();
    await Promise.resolve();
    expect(rebuild).toHaveBeenCalledTimes(1);
    release();
    await vi.waitFor(() => expect(rebuild).toHaveBeenCalledTimes(2));
    fake.stop();
    await loop;
    expect(overlapped).toBe(false);
  });

  it("waits for a rebuild that is running when it is stopped", async () => {
    const fake = fakeWatch();
    let finished = false;
    let release: () => void = () => {};
    const rebuild = vi.fn(async () => {
      await new Promise<void>((resolve) => {
        release = resolve;
      });
      finished = true;
    });
    const loop = watchLoop({ watch: fake.watch, directory: "/d", rebuild });
    fake.change();
    await vi.waitFor(() => expect(rebuild).toHaveBeenCalledTimes(1));
    fake.stop();
    release();
    await loop;
    expect(finished).toBe(true);
  });
});
