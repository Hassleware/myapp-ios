// Remembers which checklist boxes you have ticked, in this browser only. Nothing is sent anywhere.
document.querySelectorAll(".task-list-item input[type=checkbox]").forEach((box, index) => {
  const key = "gamelab-handbook:" + location.pathname + ":" + index;
  try {
    box.checked = localStorage.getItem(key) === "1";
    box.addEventListener("change", () => localStorage.setItem(key, box.checked ? "1" : "0"));
  } catch (error) {
    // Private windows may refuse storage. The boxes still tick; they are just forgotten on reload.
  }
});
