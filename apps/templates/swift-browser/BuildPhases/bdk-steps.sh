# shellcheck shell=bash
# Sourced by the other scripts in this directory; not run on its own.
#
# bdk_run_steps <step>...: runs `bdk build` with the steps the CLI has. A step this CLI does not have yet is
# reported as an Xcode warning and left out, so a template written for the whole of sdk/cli.md section 5 builds
# with a CLI that has some of it. Fails, as an Xcode error, when BDK_CLI names no executable.
#
# Xcode sets the build settings (SRCROOT, DERIVED_FILE_DIR, CODESIGNING_FOLDER_PATH, ...) as environment
# variables. BDK_CLI is the one the template defines: the path of the bdk executable in the resolved package's
# artifacts.

bdk_run_steps() {
  if [[ -z "${BDK_CLI:-}" || ! -x "${BDK_CLI}" ]]; then
    echo "error: BDK_CLI does not name the bdk executable (${BDK_CLI:-unset}). Set the BDK_CLI build setting." >&2
    return 1
  fi
  local known steps=() step
  known="$("${BDK_CLI}" --help | sed -n 's/^ *build  *Run build steps\. Steps: \(.*\)\.$/\1/p' | tr -d ',')"
  for step in "$@"; do
    if [[ " ${known} " == *" ${step} "* ]]; then
      steps+=("${step}")
    else
      echo "warning: this bdk has no \`build ${step}\` step yet (it has: ${known}); skipping it." >&2
    fi
  done
  if [[ ${#steps[@]} -eq 0 ]]; then
    echo "error: none of the steps $* exist in this bdk." >&2
    return 1
  fi
  "${BDK_CLI}" build "${steps[@]}" --project "${SRCROOT}"
}
