// Fills the first password field on the page, and the text or email field before it, when the worker or the popup
// sends credentials. The page's own scripts see ordinary input and change events.
function setValue(input, value) {
  input.focus();
  input.value = value;
  input.dispatchEvent(new Event("input", { bubbles: true }));
  input.dispatchEvent(new Event("change", { bubbles: true }));
}

function fill({ username, password }) {
  const passwordField = document.querySelector("input[type=password]");
  if (!passwordField) return false;
  const fields = [...document.querySelectorAll("input[type=text], input[type=email]")];
  const usernameField = fields
    .filter(
      (field) => passwordField.compareDocumentPosition(field) & Node.DOCUMENT_POSITION_PRECEDING,
    )
    .at(-1);
  if (usernameField) setValue(usernameField, username);
  setValue(passwordField, password);
  document.documentElement.dataset.bdkPasswords = "filled";
  return true;
}

chrome.runtime.onMessage.addListener((message, _sender, reply) => {
  if (message?.fill) reply({ filled: fill(message.fill) });
  return false;
});
