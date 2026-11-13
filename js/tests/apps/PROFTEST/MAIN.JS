// SPDX-License-Identifier: MIT
// Drives the <game> element through the test profile (tests/host/profile_test.c).
// Phase 1 plays in the direct 8 bpp mode, phase 2 as a normal element (draw hook).
const g = document.createElement('game');
g.id = 'g'; g.style.position = 'absolute'; g.style.left = 0; g.style.top = 0; g.style.width = 320; g.style.height = 200;
document.body.appendChild(g);
const im = document.createElement('img');
im.src = '/sd/GAMES/PIC'; im.style.position = 'absolute'; im.style.left = 400; im.style.top = 0; im.style.width = 160; im.style.height = 100;
document.body.appendChild(im);
g.src = '/sd/GAMES/BAD';
try { g.play(); } catch (e) { console.log('APP refused ' + e.name + ': ' + e.message); }
g.src = '/sd/GAMES/T1';
let phase = 1;
g.addEventListener('ended', () => {
  console.log('APP ended ' + phase + ' ticks ' + g.ticks + ' playing ' + g.playing);
  if (phase++ === 1) { screen.gameLowres(false); try { g.play(); } catch (e) { sys.exit(0); } } else sys.exit(0);
});
try { g.play(); } catch (e) { console.log('APP first play: ' + e.name); }
console.log('APP playing ' + g.playing);
setTimeout(() => g.press(31, 1), 100);
