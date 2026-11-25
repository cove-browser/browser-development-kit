import "../shared/style.css";
import { render } from "preact";
import { App } from "../shared/App";
import { load, title } from "../shared/starters/{{starter}}";

const root = document.querySelector("#app");
if (root !== null) render(<App title={title} load={load} />, root);
