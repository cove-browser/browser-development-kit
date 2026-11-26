/** A line of versions: from `from`, up to but not including `before`. */
export interface VersionRange {
  readonly from: string;
  readonly before: string;
}

/**
 * The versions of Vite, and of the Vite plugins `bdk init`'s templates pin, that this preset is built and checked
 * with (`apps/templates/pages/versions.json`). `bdk doctor` warns about a pin outside them
 * (`doctor.vite_dependency_unsupported`). Change a line here when the templates move to a new major version and
 * the preset has been checked with it.
 */
export const SUPPORTED_DEPENDENCIES: Readonly<Record<string, VersionRange>> = {
  vite: { from: "8.0.0", before: "9.0.0" },
  "@vitejs/plugin-react": { from: "6.0.0", before: "7.0.0" },
  "@preact/preset-vite": { from: "2.10.0", before: "3.0.0" },
  "@tailwindcss/vite": { from: "4.0.0", before: "5.0.0" },
};
