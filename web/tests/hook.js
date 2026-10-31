// Test helper: log pointer lock requests, rejections and state changes.
(() => {
  const original = Element.prototype.requestPointerLock;
  Element.prototype.requestPointerLock = function (...args) {
    console.log('REQLOCK active=' + (navigator.userActivation && navigator.userActivation.isActive));
    const p = original.apply(this, args);
    if (p && p.catch) p.catch(e => console.log('REJECT ' + e.name + ' ' + e.message));
    return p;
  };
  document.addEventListener('pointerlockerror', () => console.log('LOCKERROR'));
  document.addEventListener('pointerlockchange', () => console.log('LOCKCHANGE ' + !!document.pointerLockElement));
  console.log('HOOKED');
})();
