chrome.commands.onCommand.addListener((name, tab) => {
  chrome.scripting.executeScript({
    target: { tabId: tab.id },
    func: (command, id) => {
      document.title = "command:" + command + ":" + id;
    },
    args: [name, tab.id],
  });
});
