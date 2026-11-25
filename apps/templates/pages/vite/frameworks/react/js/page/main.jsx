import "../shared/style.css";
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "../shared/App";
import { load, title } from "../shared/starters/{{starter}}";

const root = document.querySelector("#app");
if (root !== null) {
  createRoot(root).render(
    <StrictMode>
      <App title={title} load={load} />
    </StrictMode>,
  );
}
