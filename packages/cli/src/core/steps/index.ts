import { automationStep } from "./automation.js";
import { configStep } from "./config.js";
import { devtoolsStep } from "./devtools.js";
import { signStep } from "./sign.js";
import type { Step } from "./step.js";

/** The `bdk build` steps that exist, in run order. Register a step only when it works. */
export const BUILD_STEPS: readonly Step[] = [configStep, automationStep, devtoolsStep, signStep];
