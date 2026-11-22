import { type Commands, createCommands } from "./commands.js";
import type { Connection } from "./connection.js";
import { createEvents, type Events } from "./events.js";
import { NAMESPACES, type NamespaceName, namespace } from "./namespaces.js";
import { type Contents, createContents } from "./page.js";
import { createScripts, type Scripts } from "./scripts.js";
import type { Services } from "./services.js";

/** Everything a connection offers apart from the session. */
export type Api = {
  contents: Contents;
  scripts: Scripts;
  commands: Commands;
  events: Events;
} & Pick<Services, NamespaceName>;

/** Builds the whole API over a connection that is resolved on first use. */
export function createApi(connection: () => Connection): Api {
  const namespaces = Object.fromEntries(
    NAMESPACES.map((name) => [name, namespace(name, connection)]),
  ) as Pick<Services, NamespaceName>;
  return {
    ...namespaces,
    contents: createContents(connection),
    scripts: createScripts(connection),
    commands: createCommands(connection),
    events: createEvents(connection),
  };
}
