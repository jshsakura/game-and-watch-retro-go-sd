/**
 * Game & Watch edition switch — Zelda (green) ↔ Mario (red), like game-and-what.
 * Sets html[data-edition] and injects a pill toggle into the navbar. Persisted
 * to localStorage. Replaces the plain light/dark toggle.
 */
import ExecutionEnvironment from '@docusaurus/ExecutionEnvironment';

const KEY = 'gw-edition';
const DEFAULT = 'zelda';

function current() {
  try {
    return localStorage.getItem(KEY) === 'mario' ? 'mario' : DEFAULT;
  } catch (e) {
    return DEFAULT;
  }
}

function label(ed) {
  return ed === 'mario' ? '🔴 Mario' : '🟢 Zelda';
}

function apply(ed) {
  document.documentElement.setAttribute('data-edition', ed);
  try {
    localStorage.setItem(KEY, ed);
  } catch (e) {}
  const btn = document.getElementById('gw-edition-toggle');
  if (btn) {
    btn.setAttribute('data-edition', ed);
    btn.textContent = label(ed);
    btn.setAttribute(
      'title',
      ed === 'mario'
        ? 'Mario edition (red) — click for Zelda'
        : 'Zelda edition (green) — click for Mario',
    );
  }
}

function ensureButton() {
  if (document.getElementById('gw-edition-toggle')) return;
  const right = document.querySelector('.navbar__items--right');
  if (!right) return;
  const btn = document.createElement('button');
  btn.id = 'gw-edition-toggle';
  btn.className = 'gw-edition-toggle';
  btn.type = 'button';
  btn.setAttribute('aria-label', 'Switch Game & Watch edition');
  btn.addEventListener('click', () => {
    const cur = document.documentElement.getAttribute('data-edition') || DEFAULT;
    apply(cur === 'mario' ? 'zelda' : 'mario');
  });
  right.insertBefore(btn, right.firstChild);
  apply(current());
}

// Set the attribute as early as possible to minimise a flash of the default.
if (ExecutionEnvironment.canUseDOM) {
  document.documentElement.setAttribute('data-edition', current());
}

export function onRouteDidUpdate() {
  ensureButton();
}
