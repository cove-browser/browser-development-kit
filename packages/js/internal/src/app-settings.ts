/**
 * The application's settings: key to value type. `bdk build codegen` fills it by declaration merging
 * (`@bdk/internal/app.d.ts`), so a misspelt application key is a compile error.
 */
// biome-ignore lint/suspicious/noEmptyInterface: filled by declaration merging
export interface AppSettings {}

/** A setting key. Browser keys are not catalogued yet, so any string is accepted; application keys complete. */
export type SettingKey = (keyof AppSettings & string) | (string & {});
