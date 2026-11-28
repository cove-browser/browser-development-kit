import "../shared/style.css";
import { render } from "../shared/render";
import { load, title } from "../shared/starters/{{starter}}";

void render(document.querySelector("#app"), title, load);
