// A tiny shell for sandbox-kernel, written like a small web page: the layout
// is INDEX.HTM, and this script fills it in. Arrow keys move between buttons;
// Enter clicks.
const here = '/sd/OS/APPS/HELLO';
// 960x540 is exactly half of 1080p: the GPU doubles each pixel on a 1080p
// screen. A kernel without screen modes keeps 640x480.
try {
  screen.setMode(960, 540, 16);
} catch (e) {
  console.log('staying at ' + screen.width + 'x' + screen.height);
}
document.body.style.background = '#0b1020';
document.body.innerHTML = fs.readFile(here + '/INDEX.HTM');
const page = document.getElementById('page');

function about() {
  page.innerHTML =
    '<text style="color:#ffffff">This page is JavaScript on a Raspberry Pi Zero W.</text>' +
    '<text style="color:#8a96b8">Edit /OS/APPS/HELLO/MAIN.JS and INDEX.HTM to make it yours.</text>';
}

function games() {
  page.innerHTML = '<div id="grid" style="gap:10px"></div>';
  const grid = document.getElementById('grid');
  let names = [];
  try {
    names = fs.readdir('/sd/GAMES').slice(0, 6);
  } catch (e) {
    page.textContent = 'No /GAMES folder on the card.';
    return;
  }
  let row = null;
  names.forEach((name, i) => {
    if (i % 3 === 0) {
      row = document.createElement('div');
      row.style.display = 'flex';
      row.style.flexDirection = 'row';
      row.style.gap = 10;
      grid.appendChild(row);
    }
    const card = document.createElement('button');
    card.style.width = 300;
    card.style.textAlign = 'left';
    card.style.background = '#1d2b53';
    const pic = document.createElement('img');
    pic.src = '/sd/GAMES/' + name;
    pic.style.width = 288;
    pic.style.height = 180;
    const label = document.createElement('text');
    label.textContent = name;
    label.style.fontSize = '18px';
    card.appendChild(pic);
    card.appendChild(label);
    card.addEventListener('click', () => play(name));
    row.appendChild(card);
  });
  const first = grid.firstChild && grid.firstChild.firstChild;
  if (first) first.focus();
}

function play(name) {
  const game = document.createElement('game');
  game.style.position = 'absolute';
  game.src = '/sd/GAMES/' + name;
  game.addEventListener('ended', () => {
    game.remove();
    games();
  });
  document.body.appendChild(game);
  game.play();
}

document.getElementById('games').addEventListener('click', games);
document.getElementById('about').addEventListener('click', about);
document.getElementById('games').focus();
about();

const clock = document.getElementById('clock');
setInterval(() => {
  const s = Math.floor(performance.now() / 1000);
  clock.textContent = 'up ' + Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
}, 1000);
console.log('hello OS ready');
