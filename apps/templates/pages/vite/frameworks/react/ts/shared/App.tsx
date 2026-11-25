// Shared by every page at build time. React builds the DOM without HTML strings, which the content
// security policy (Trusted Types) forbids; do not use dangerouslySetInnerHTML.
import { useEffect, useState } from "react";

export interface Item {
  label: string;
  detail: string;
  href?: string;
}

export function App({ title, load }: { title: string; load: () => Promise<Item[]> }) {
  const [items, setItems] = useState<Item[] | undefined>();
  const [error, setError] = useState<string | undefined>();
  useEffect(() => {
    load().then(setItems, (e: unknown) => setError(e instanceof Error ? e.message : String(e)));
  }, [load]);
  return (
    <main className="mx-auto max-w-2xl p-4">
      <h1 className="text-2xl font-semibold">{title}</h1>
      <p role="status" className={error ? "error text-red-600" : "muted opacity-70"}>
        {error
          ? `Could not load: ${error}`
          : items === undefined
            ? "Loading"
            : items.length === 0
              ? "Nothing here yet."
              : ""}
      </p>
      <ul>
        {(items ?? []).map((item, index) => (
          <li key={index} className="flex flex-col border-b py-2">
            {item.href ? <a href={item.href}>{item.label}</a> : <span>{item.label}</span>}
            <small className="opacity-70">{item.detail}</small>
          </li>
        ))}
      </ul>
    </main>
  );
}
