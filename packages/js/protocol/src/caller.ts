export type CallerKind = "swift" | "page" | "session";

/** The answer of `runtime.caller`: who is calling and what they may do. */
export interface CallerInfo {
  readonly kind: CallerKind;
  /** The logical page or session name. */
  readonly name: string;
  readonly profile?: string;
  readonly capabilities: readonly string[];
  /** The branded scheme of internal pages, such as `cove`. Present for a page. */
  readonly internalScheme?: string;
}

/** The caller of an internal page. */
export type Page = CallerInfo & { readonly kind: "page" };
