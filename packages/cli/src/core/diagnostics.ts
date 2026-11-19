/** A finding reported by any `bdk` step. Codes are stable (configuration.md section 9). */
export interface Diagnostic {
  code: string;
  severity: "error" | "warning";
  message: string;
  file?: string;
  line?: number;
  column?: number;
  fix?: string;
}
