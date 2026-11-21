/** A finding reported by any `bdk` step. Codes are stable (configuration.md section 9). */
export interface Diagnostic {
  code: string;
  /** `note` is information that needs no action, such as an input that is optional and absent. */
  severity: "error" | "warning" | "note";
  message: string;
  file?: string;
  line?: number;
  column?: number;
  fix?: string;
}
