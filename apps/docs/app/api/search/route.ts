import { createFromSource } from "fumadocs-core/search/server";
import { source } from "@/lib/source";

// Written once at build time: the static export serves the index as a file.
export const revalidate = false;

export const { staticGET: GET } = createFromSource(source, { language: "english" });
