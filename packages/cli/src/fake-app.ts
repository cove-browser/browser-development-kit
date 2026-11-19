import { mkdir, realpath, symlink, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { tempDir } from "./test-support.js";

export const FAKE_HELPERS = [
  "Fake Helper (Alerts).app",
  "Fake Helper (Aperitif Alerts).app",
  "Fake Helper (Aperitif GPU).app",
  "Fake Helper (Aperitif Renderer).app",
  "Fake Helper (Aperitif).app",
  "Fake Helper (GPU).app",
  "Fake Helper (Renderer).app",
  "Fake Helper.app",
];

export const FAKE_EXECUTABLES = [
  "app_mode_loader",
  "chrome_crashpad_handler",
  "web_app_shortcut_copier",
];

/** An application bundle laid out like the packaged runtime, with empty files: no real code in it. */
export async function fakeApp(): Promise<{ appPath: string; versionDir: string }> {
  const root = await tempDir("bdk-fakeapp-");
  const appPath = join(root, "Demo.app");
  const framework = join(appPath, "Contents", "Frameworks", "Fake Framework.framework");
  const versionDir = join(framework, "Versions", "1");
  await mkdir(join(versionDir, "Resources"), { recursive: true });
  await mkdir(join(versionDir, "Libraries"), { recursive: true });
  await mkdir(join(versionDir, "Helpers"), { recursive: true });
  await symlink("1", join(framework, "Versions", "Current"));
  await symlink("Versions/Current/Helpers", join(framework, "Helpers"));
  await writeFile(join(appPath, "Contents", "Info.plist"), "");
  await writeFile(join(versionDir, "Resources", "Info.plist"), "");
  for (const lib of ["libvulkan.dylib", "libaperitif.dylib", "data.json"]) {
    await writeFile(join(versionDir, "Libraries", lib), "");
  }
  for (const exe of FAKE_EXECUTABLES) {
    await writeFile(join(versionDir, "Helpers", exe), "");
  }
  for (const helper of FAKE_HELPERS) {
    await mkdir(join(versionDir, "Helpers", helper, "Contents"), { recursive: true });
    await writeFile(join(versionDir, "Helpers", helper, "Contents", "Info.plist"), "");
  }
  return { appPath, versionDir: await realpath(versionDir) };
}
