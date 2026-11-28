import { createMDX } from "fumadocs-mdx/next";

const withMDX = createMDX();

/** The path the site is published under, such as `/browser-development-kit` on GitHub Pages; empty by default. */
const basePath = process.env.BDK_DOCS_BASE_PATH ?? "";

/**
 * A static export: `pnpm docs:build` writes the whole site to `out/`, which any static host serves.
 *
 * @type {import('next').NextConfig}
 */
const config = {
  output: "export",
  reactStrictMode: true,
  trailingSlash: true,
  basePath,
  images: { unoptimized: true },
  // Client code (the search dialog) fetches the index under the same base path.
  env: { NEXT_PUBLIC_BDK_DOCS_BASE_PATH: basePath },
};

export default withMDX(config);
