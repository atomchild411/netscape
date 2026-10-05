// qjs_dom.js --- libmocha on QuickJS: the DOM's JavaScript half.
//
// Built into the program as qjs_dom_js.h (mkjsheader.py): edit this file,
// then run "python3 mkjsheader.py qjs_dom.js qjs_dom_js.h qjs_dom_js".
//
// Nodes are objects of one native class whose prototype C picks from the
// table given to D.protos (by node type, and by tag name for elements).
// D (__ns.dom) is the C half: the tree, attributes, selectors, HTML.
(function(g, ns) {
'use strict';
var D = ns.dom;
var LS = Symbol('listeners'), HS = Symbol('handlers'), CL = Symbol('classList'),
    ST = Symbol('style'), DS = Symbol('dataset'), DONE = Symbol('scriptDone'),
    VAL = Symbol('value');

function log(m) { ns.log(m); }
function report(where, x) {
  log('uncaught in ' + where + ': ' + x + (x && x.stack ? '\n' + x.stack : ''));
}

function klass(name, parent, ctor) {
  var C = ctor || function() { throw new TypeError('Illegal constructor'); };
  if (parent) {
    C.prototype = Object.create(parent.prototype);
    Object.setPrototypeOf(C, parent);
  }
  Object.defineProperty(C.prototype, 'constructor', { value: C, writable: true, configurable: true });
  Object.defineProperty(C, 'name', { value: name });
  Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
  g[name] = C;
  return C;
}
function getters(proto, o) {
  Object.keys(o).forEach(function(k) {
    var d = o[k];
    Object.defineProperty(proto, k, typeof d === 'function' ? { get: d, configurable: true } :
      { get: d[0], set: d[1], configurable: true });
  });
}
function methods(proto, o) {
  Object.keys(o).forEach(function(k) {
    Object.defineProperty(proto, k, { value: o[k], writable: true, configurable: true });
  });
}
function list(a) {
  a.item = function(i) { return a[i] === undefined ? null : a[i]; };
  a.namedItem = function(n) {
    for (var i = 0; i < a.length; i++)
      if (a[i].id === n || a[i].getAttribute('name') === n) return a[i];
    return null;
  };
  return a;
}

// ---- events -----------------------------------------------------------------

var Event = klass('Event', null, function Event(type, init) {
  if (!(this instanceof Event)) throw new TypeError("Constructor Event requires 'new'");
  init = init || {};
  this.type = String(type);
  this.bubbles = !!init.bubbles;
  this.cancelable = !!init.cancelable;
  this.composed = !!init.composed;
  this.defaultPrevented = false;
  this.target = this.currentTarget = this.srcElement = null;
  this.eventPhase = 0;
  this.timeStamp = Date.now();
  this.isTrusted = false;
  this._stop = this._stopNow = false;
});
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
methods(Event.prototype, {
  preventDefault: function() { if (this.cancelable) this.defaultPrevented = true; },
  stopPropagation: function() { this._stop = true; },
  stopImmediatePropagation: function() { this._stop = this._stopNow = true; },
  composedPath: function() { return this._path ? this._path.slice() : []; },
  initEvent: function(t, b, c) { this.type = String(t); this.bubbles = !!b; this.cancelable = !!c; }
});
getters(Event.prototype, {
  returnValue: [function() { return !this.defaultPrevented; },
                function(v) { if (!v) this.preventDefault(); }],
  cancelBubble: [function() { return this._stop; }, function(v) { if (v) this._stop = true; }]
});
function subEvent(name, parent, fields) {
  return klass(name, parent, function(type, init) {
    parent.call(this, type, init);
    init = init || {};
    for (var k in fields) this[k] = init[k] !== undefined ? init[k] : fields[k];
  });
}
var UIEvent = subEvent('UIEvent', Event, { view: null, detail: 0 });
var MouseEvent = subEvent('MouseEvent', UIEvent, { screenX: 0, screenY: 0, clientX: 0,
  clientY: 0, pageX: 0, pageY: 0, offsetX: 0, offsetY: 0, ctrlKey: false, shiftKey: false,
  altKey: false, metaKey: false, button: 0, buttons: 0, relatedTarget: null, which: 1 });
subEvent('PointerEvent', MouseEvent, { pointerId: 1, pointerType: 'mouse', isPrimary: true });
subEvent('WheelEvent', MouseEvent, { deltaX: 0, deltaY: 0, deltaZ: 0, deltaMode: 0 });
subEvent('KeyboardEvent', UIEvent, { key: '', code: '', keyCode: 0, charCode: 0, which: 0,
  ctrlKey: false, shiftKey: false, altKey: false, metaKey: false, repeat: false, location: 0 });
subEvent('FocusEvent', UIEvent, { relatedTarget: null });
subEvent('InputEvent', UIEvent, { data: null, inputType: '', isComposing: false });
subEvent('CustomEvent', Event, { detail: null });
subEvent('ErrorEvent', Event, { message: '', filename: '', lineno: 0, colno: 0, error: null });
subEvent('PopStateEvent', Event, { state: null });
subEvent('HashChangeEvent', Event, { oldURL: '', newURL: '' });
subEvent('ProgressEvent', Event, { lengthComputable: false, loaded: 0, total: 0 });
subEvent('MessageEvent', Event, { data: null, origin: '', source: null, ports: [] });
subEvent('StorageEvent', Event, { key: null, oldValue: null, newValue: null, url: '' });
subEvent('PageTransitionEvent', Event, { persisted: false });
subEvent('TransitionEvent', Event, { propertyName: '', elapsedTime: 0 });
subEvent('AnimationEvent', Event, { animationName: '', elapsedTime: 0 });
g.CustomEvent.prototype.initCustomEvent = function(t, b, c, d) {
  this.initEvent(t, b, c); this.detail = d;
};

var EventTarget = klass('EventTarget', null, function EventTarget() {});

function listenersOf(t, create) {
  var l = t[LS];
  if (!l && create) Object.defineProperty(t, LS, { value: l = {}, configurable: true });
  return l;
}
// The on... handler of T for TYPE: a property, else the attribute's code.
function handlerOf(t, type) {
  var name = 'on' + type, own = Object.getOwnPropertyDescriptor(t, name);
  if (own) return typeof own.value === 'function' ? own.value : null;
  if (t === g && typeof g[name] === 'function') return g[name];
  if (t.nodeType !== 1) return null;
  var src = D.attr(t, name);
  if (src === null) return null;
  var h = t[HS];
  if (!h) Object.defineProperty(t, HS, { value: h = {}, configurable: true });
  if (h[name] && h[name].src === src) return h[name].fn;
  var fn = null;
  try {
    fn = new Function('event', 'with (this.ownerDocument || document) { with (this.form || {}) { with (this) {\n' +
                      src + '\n} } }');
  } catch (x) { report(name + ' attribute', x); }
  h[name] = { src: src, fn: fn };
  return fn;
}
function invoke(t, ev, f, isHandler) {
  try {
    var r = typeof f === 'function' ? f.call(t, ev) : f.handleEvent(ev);
    if (isHandler && r === false && ev.type !== 'mouseover') ev.preventDefault();
  } catch (x) { report(ev.type + ' listener', x); }
}
function fire(t, ev, phase) {
  ev.currentTarget = t;
  ev.eventPhase = phase;
  var l = listenersOf(t, false), a = l && l[ev.type];
  if (phase !== 1) {
    var h = handlerOf(t, ev.type);
    if (h) invoke(t, ev, h, true);
  }
  if (!a) return;
  a = a.slice();
  for (var i = 0; i < a.length && !ev._stopNow; i++) {
    var e = a[i];
    if (e.removed) continue;
    if ((phase === 1 && !e.capture) || (phase === 3 && e.capture)) continue;
    if (e.once) EventTarget.prototype.removeEventListener.call(t, ev.type, e.fn, e.capture);
    invoke(t, ev, e.fn, false);
  }
}
function dispatch(target, ev) {
  var path = [], n = target;
  while (n) {
    path.push(n);
    if (n === g) break;
    n = n.nodeType === 9 ? g : (n.nodeType ? D.parent(n) : null);
  }
  ev.target = ev.srcElement = target;
  ev._path = path;
  ev._stop = ev._stopNow = false;
  for (var i = path.length - 1; i > 0 && !ev._stop; i--) fire(path[i], ev, 1);
  if (!ev._stop) fire(target, ev, 2);
  if (ev.bubbles)
    for (i = 1; i < path.length && !ev._stop; i++) fire(path[i], ev, 3);
  ev.currentTarget = null;
  ev.eventPhase = 0;
  return !ev.defaultPrevented;
}
methods(EventTarget.prototype, {
  addEventListener: function(type, fn, opts) {
    if (!fn) return;
    if (this == null) return EventTarget.prototype.addEventListener.call(g, type, fn, opts);
    var capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    var l = listenersOf(this, true), a = l[type] || (l[type] = []);
    for (var i = 0; i < a.length; i++)
      if (a[i].fn === fn && a[i].capture === capture) return;
    var e = { fn: fn, capture: capture, once: !!(opts && opts.once) }, self = this;
    a.push(e);
    if (opts && opts.signal && opts.signal.addEventListener)
      opts.signal.addEventListener('abort', function() {
        EventTarget.prototype.removeEventListener.call(self, type, fn, capture);
      });
  },
  removeEventListener: function(type, fn, opts) {
    if (this == null) return EventTarget.prototype.removeEventListener.call(g, type, fn, opts);
    var capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    var l = listenersOf(this, false), a = l && l[type];
    if (!a) return;
    for (var i = 0; i < a.length; i++)
      if (a[i].fn === fn && a[i].capture === capture) {
        a[i].removed = true;
        a.splice(i, 1);
        return;
      }
  },
  dispatchEvent: function(ev) {
    if (this == null) return EventTarget.prototype.dispatchEvent.call(g, ev);
    if (typeof ev === 'string') ev = new Event(ev);
    ev.isTrusted = false;
    return dispatch(this, ev);
  }
});
// the window is an EventTarget too
['addEventListener', 'removeEventListener', 'dispatchEvent'].forEach(function(k) {
  g[k] = EventTarget.prototype[k];
});
var AbortSignal = klass('AbortSignal', EventTarget, function AbortSignal() {
  this.aborted = false; this.reason = undefined; this.onabort = null;
});
AbortSignal.prototype.throwIfAborted = function() { if (this.aborted) throw this.reason; };
AbortSignal.abort = function(r) { var s = new AbortSignal(); s.aborted = true; s.reason = r; return s; };
AbortSignal.timeout = function(ms) { var c = new g.AbortController(); setTimeout(function() { c.abort(); }, ms); return c.signal; };
klass('AbortController', null, function AbortController() { this.signal = new AbortSignal(); });
g.AbortController.prototype.abort = function(r) {
  var s = this.signal;
  if (s.aborted) return;
  s.aborted = true;
  s.reason = r === undefined ? new Error('AbortError') : r;
  dispatch(s, new Event('abort'));
};

// ---- nodes --------------------------------------------------------------------

var Node = klass('Node', EventTarget);
var consts = { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4,
  PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10,
  DOCUMENT_FRAGMENT_NODE: 11, DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2,
  DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_CONTAINS: 8, DOCUMENT_POSITION_CONTAINED_BY: 16 };
for (var k in consts) Node[k] = Node.prototype[k] = consts[k];

var document;		// the libdom document's object, below
var mutationHook = null;	// MutationObserver's

function toNode(x) {
  return (x && typeof x === 'object' && x instanceof Node) ? x : D.createText(String(x));
}
function isConnected(n) { return document ? D.contains(document, n) : false; }
// Scripts put into the document run then (once each).
function scriptsIn(n) {
  if (!n || !isConnected(n)) return;
  if (n.nodeType === 1 && n.tagName === 'SCRIPT') runScript(n);
  else if (n.nodeType === 1 || n.nodeType === 11) {
    var s = D.byTag(n, 'script');
    for (var i = 0; i < s.length; i++) runScript(s[i]);
  }
}
function inserted(parent, nodes) {
  for (var i = 0; i < nodes.length; i++) scriptsIn(nodes[i]);
  if (isConnected(parent)) ceConnected(nodes, true);
  if (mutationHook) mutationHook('childList', parent, nodes, []);
}
function removed(parent, node) {
  if (isConnected(parent)) ceConnected([node], false);
  if (mutationHook) mutationHook('childList', parent, [], [node]);
}
function kidsOf(n) { return n.nodeType === 11 ? D.kids(n) : [n]; }

getters(Node.prototype, {
  nodeType: function() { return D.type(this); },
  nodeName: function() { return D.name(this); },
  nodeValue: [function() { var t = D.type(this); return t === 3 || t === 8 || t === 4 ? D.data(this) : null; },
              function(v) { var t = D.type(this); if (t === 3 || t === 8 || t === 4) D.setData(this, String(v)); }],
  parentNode: function() { return D.parent(this); },
  parentElement: function() { var p = D.parent(this); return p && D.type(p) === 1 ? p : null; },
  childNodes: function() { return list(D.kids(this)); },
  firstChild: function() { return D.first(this); },
  lastChild: function() { return D.last(this); },
  nextSibling: function() { return D.next(this); },
  previousSibling: function() { return D.prev(this); },
  ownerDocument: function() { return D.type(this) === 9 ? null : document; },
  isConnected: function() { return isConnected(this); },
  textContent: [function() { var t = D.type(this); return t === 9 ? null : D.text(this); },
                function(v) {
                  if (D.type(this) === 9) return;
                  var old = mutationHook ? D.kids(this) : null;
                  D.setText(this, v == null ? '' : String(v));
                  if (mutationHook) mutationHook('childList', this, D.kids(this), old);
                }],
  baseURI: function() { return ns.url(); }
});
methods(Node.prototype, {
  appendChild: function(c) {
    var nodes = kidsOf(c);
    D.insert(this, c, null);
    inserted(this, nodes);
    return c;
  },
  insertBefore: function(c, ref) {
    var nodes = kidsOf(c);
    D.insert(this, c, ref || null);
    inserted(this, nodes);
    return c;
  },
  removeChild: function(c) { D.remove(this, c); removed(this, c); return c; },
  replaceChild: function(n, old) {
    var nodes = kidsOf(n);
    D.replace(this, n, old);
    inserted(this, nodes);
    removed(this, old);
    return old;
  },
  cloneNode: function(deep) { return D.clone(this, !!deep); },
  hasChildNodes: function() { return D.first(this) !== null; },
  contains: function(o) { return o ? D.contains(this, o) : false; },
  getRootNode: function() { var n = this, p; while ((p = D.parent(n))) n = p; return n; },
  isSameNode: function(o) { return this === o; },
  isEqualNode: function(o) {
    return !!o && D.type(this) === D.type(o) && D.html(this, true) === D.html(o, true) &&
      D.data(this) === D.data(o);
  },
  normalize: function() {},
  compareDocumentPosition: function(o) {
    if (o === this) return 0;
    if (D.contains(this, o)) return 20;
    if (D.contains(o, this)) return 10;
    if (this.getRootNode() !== o.getRootNode()) return 37;
    // which comes first in document order
    function chain(n) { var a = []; while (n) { a.unshift(n); n = D.parent(n); } return a; }
    var a = chain(this), b = chain(o), i = 0;
    while (a[i] === b[i]) i++;
    for (var s = D.next(a[i]); s; s = D.next(s)) if (s === b[i]) return 4;
    return 2;
  },
  lookupNamespaceURI: function() { return 'http://www.w3.org/1999/xhtml'; },
  isDefaultNamespace: function(n) { return n === 'http://www.w3.org/1999/xhtml'; }
});

var CharacterData = klass('CharacterData', Node);
getters(CharacterData.prototype, {
  data: [function() { return D.data(this); }, function(v) { D.setData(this, String(v)); }],
  length: function() { return D.data(this).length; },
  nextElementSibling: function() { return nextEl(this); },
  previousElementSibling: function() { return prevEl(this); }
});
methods(CharacterData.prototype, {
  appendData: function(s) { D.setData(this, D.data(this) + s); },
  substringData: function(o, n) { return D.data(this).substr(o, n); },
  deleteData: function(o, n) { var d = D.data(this); D.setData(this, d.slice(0, o) + d.slice(o + n)); },
  insertData: function(o, s) { var d = D.data(this); D.setData(this, d.slice(0, o) + s + d.slice(o)); },
  replaceData: function(o, n, s) { var d = D.data(this); D.setData(this, d.slice(0, o) + s + d.slice(o + n)); },
  remove: function() { var p = D.parent(this); if (p) p.removeChild(this); },
  before: function() { var p = D.parent(this); if (p) for (var i = 0; i < arguments.length; i++) p.insertBefore(toNode(arguments[i]), this); },
  after: function() { var p = D.parent(this), n = D.next(this); if (p) for (var i = 0; i < arguments.length; i++) p.insertBefore(toNode(arguments[i]), n); },
  replaceWith: function() { var p = D.parent(this); if (!p) return; this.before.apply(this, arguments); p.removeChild(this); }
});
var Text = klass('Text', CharacterData, function Text(s) { return D.createText(s === undefined ? '' : String(s)); });
getters(Text.prototype, { wholeText: function() { return D.data(this); } });
Text.prototype.splitText = function(o) {
  var d = D.data(this), t = D.createText(d.slice(o));
  D.setData(this, d.slice(0, o));
  var p = D.parent(this);
  if (p) D.insert(p, t, D.next(this));
  return t;
};
var Comment = klass('Comment', CharacterData, function Comment(s) { return D.createComment(s === undefined ? '' : String(s)); });
var DocumentFragment = klass('DocumentFragment', Node, function DocumentFragment() { return D.createFragment(); });

function nextEl(n) { for (n = D.next(n); n && D.type(n) !== 1; n = D.next(n)); return n; }
function prevEl(n) { for (n = D.prev(n); n && D.type(n) !== 1; n = D.prev(n)); return n; }

// what elements, documents and fragments share
var parentNodeMixin = {
  children: function() { return list(D.elementKids(this)); },
  childElementCount: function() { return D.elementKids(this).length; },
  firstElementChild: function() { var n = D.first(this); while (n && D.type(n) !== 1) n = D.next(n); return n; },
  lastElementChild: function() { var n = D.last(this); while (n && D.type(n) !== 1) n = D.prev(n); return n; }
};
var parentNodeMethods = {
  querySelector: function(s) { return D.select(this, String(s), true); },
  querySelectorAll: function(s) { return list(D.select(this, String(s), false)); },
  getElementsByTagName: function(t) { return list(D.byTag(this, String(t))); },
  getElementsByTagNameNS: function(ns_, t) { return list(D.byTag(this, String(t))); },
  getElementsByClassName: function(c) { return list(D.byClass(this, String(c))); },
  append: function() {
    for (var i = 0; i < arguments.length; i++) this.appendChild(toNode(arguments[i]));
  },
  prepend: function() {
    var first = D.first(this);
    for (var i = 0; i < arguments.length; i++) this.insertBefore(toNode(arguments[i]), first);
  },
  replaceChildren: function() {
    var c;
    while ((c = D.first(this))) this.removeChild(c);
    this.append.apply(this, arguments);
  }
};
getters(DocumentFragment.prototype, parentNodeMixin);
methods(DocumentFragment.prototype, parentNodeMethods);
DocumentFragment.prototype.getElementById = function(id) {
  return D.select(this, '[id="' + String(id).replace(/["\\]/g, '\\$&') + '"]', true);
};

// ---- elements -----------------------------------------------------------------

var Element = klass('Element', Node);
getters(Element.prototype, parentNodeMixin);
methods(Element.prototype, parentNodeMethods);

function attrSetter(name) {
  return function(v) { this.setAttribute(name, v); };
}
function reflect(proto, props) {
  // props: name -> attribute (string: text; ['bool', attr]; ['num', attr, default]; ['url', attr])
  Object.keys(props).forEach(function(k) {
    var p = props[k], kind = 'str', attr = p, def = 0;
    if (Array.isArray(p)) { kind = p[0]; attr = p[1]; def = p[2] || 0; }
    var get, set;
    if (kind === 'bool') {
      get = function() { return D.hasAttr(this, attr); };
      set = function(v) { if (v) D.setAttr(this, attr, ''); else D.removeAttr(this, attr); attrChanged(this, attr); };
    } else if (kind === 'num') {
      get = function() { var v = parseInt(D.attr(this, attr), 10); return isNaN(v) ? def : v; };
      set = function(v) { D.setAttr(this, attr, String(v | 0)); attrChanged(this, attr); };
    } else if (kind === 'url') {
      get = function() { var v = D.attr(this, attr); return v === null ? '' : ns.resolve(v.trim()); };
      set = attrSetter(attr);
    } else {
      get = function() { var v = D.attr(this, attr); return v === null ? '' : v; };
      set = attrSetter(attr);
    }
    Object.defineProperty(proto, k, { get: get, set: set, configurable: true });
  });
}
function attrChanged(el, name, old) {
  var def = el[CE];
  if (def && def.observed.indexOf(name) >= 0)
    ceCall(el, 'attributeChangedCallback', [name, old === undefined ? null : old, D.attr(el, name)]);
  if (mutationHook) mutationHook('attributes', el, null, null, name);
}

function DOMTokenList(el, attr) { this._el = el; this._attr = attr; }
function tokens(tl) { var v = D.attr(tl._el, tl._attr); return v ? v.split(/\s+/).filter(Boolean) : []; }
function setTokens(tl, a) { D.setAttr(tl._el, tl._attr, a.join(' ')); attrChanged(tl._el, tl._attr); }
methods(DOMTokenList.prototype, {
  contains: function(t) { return tokens(this).indexOf(String(t)) >= 0; },
  add: function() {
    var a = tokens(this), ch = false;
    for (var i = 0; i < arguments.length; i++) {
      var t = String(arguments[i]);
      if (a.indexOf(t) < 0) { a.push(t); ch = true; }
    }
    if (ch || D.attr(this._el, this._attr) === null) setTokens(this, a);
  },
  remove: function() {
    var a = tokens(this), n = a.length;
    for (var i = 0; i < arguments.length; i++) {
      var t = String(arguments[i]), j;
      while ((j = a.indexOf(t)) >= 0) a.splice(j, 1);
    }
    if (a.length !== n) setTokens(this, a);
  },
  toggle: function(t, force) {
    t = String(t);
    var has = this.contains(t);
    if (force === undefined ? has : !force) { if (has) this.remove(t); return false; }
    if (!has) this.add(t);
    return true;
  },
  replace: function(a, b) {
    var l = tokens(this), i = l.indexOf(String(a));
    if (i < 0) return false;
    l[i] = String(b); setTokens(this, l); return true;
  },
  item: function(i) { var a = tokens(this); return i < a.length ? a[i] : null; },
  forEach: function(f, t) { tokens(this).forEach(f, t); },
  entries: function() { return tokens(this).entries(); },
  keys: function() { return tokens(this).keys(); },
  values: function() { return tokens(this).values(); },
  supports: function() { return true; },
  toString: function() { return D.attr(this._el, this._attr) || ''; }
});
DOMTokenList.prototype[Symbol.iterator] = function() { return tokens(this)[Symbol.iterator](); };
getters(DOMTokenList.prototype, {
  length: function() { return tokens(this).length; },
  value: [function() { return D.attr(this._el, this._attr) || ''; },
          function(v) { D.setAttr(this._el, this._attr, String(v)); }]
});
g.DOMTokenList = DOMTokenList;

// style: the style attribute, as a CSSStyleDeclaration
function cssName(p) {
  if (p === 'cssFloat') return 'float';
  return p.replace(/^(webkit|moz|ms)([A-Z])/, '-$1$2').replace(/[A-Z]/g, function(c) { return '-' + c.toLowerCase(); });
}
function parseStyle(s) {
  var m = new Map();
  (s || '').split(';').forEach(function(d) {
    var i = d.indexOf(':');
    if (i > 0) m.set(d.slice(0, i).trim().toLowerCase(), d.slice(i + 1).trim());
  });
  return m;
}
function writeStyle(el, m) {
  var a = [];
  m.forEach(function(v, k) { if (v !== '') a.push(k + ': ' + v); });
  if (a.length) D.setAttr(el, 'style', a.join('; ') + ';');
  else D.removeAttr(el, 'style');
  attrChanged(el, 'style');
}
var styleMethods = {
  getPropertyValue: function(el, p) { return parseStyle(D.attr(el, 'style')).get(String(p).toLowerCase()) || ''; },
  getPropertyPriority: function() { return ''; },
  setProperty: function(el, p, v) {
    var m = parseStyle(D.attr(el, 'style'));
    p = String(p).toLowerCase();
    if (v === null || v === undefined || v === '') m.delete(p); else m.set(p, String(v));
    writeStyle(el, m);
  },
  removeProperty: function(el, p) {
    var m = parseStyle(D.attr(el, 'style')), v = m.get(String(p).toLowerCase()) || '';
    m.delete(String(p).toLowerCase()); writeStyle(el, m); return v;
  },
  item: function(el, i) { return Array.from(parseStyle(D.attr(el, 'style')).keys())[i] || ''; }
};
function styleOf(el) {
  var s = el[ST];
  if (s) return s;
  s = new Proxy({}, {
    get: function(t, p) {
      if (typeof p === 'symbol') return undefined;
      if (p === 'cssText') return D.attr(el, 'style') || '';
      if (p === 'length') return parseStyle(D.attr(el, 'style')).size;
      if (styleMethods[p]) return function() {
        return styleMethods[p].apply(null, [el].concat([].slice.call(arguments)));
      };
      if (p === 'parentRule') return null;
      if (p === 'toString') return function() { return '[object CSSStyleDeclaration]'; };
      return parseStyle(D.attr(el, 'style')).get(cssName(p)) || '';
    },
    set: function(t, p, v) {
      if (typeof p === 'symbol') return true;
      if (p === 'cssText') { D.setAttr(el, 'style', String(v)); attrChanged(el, 'style'); return true; }
      styleMethods.setProperty(el, cssName(p), v);
      return true;
    },
    has: function(t, p) { return typeof p === 'string'; }
  });
  Object.defineProperty(el, ST, { value: s, configurable: true });
  return s;
}
g.CSSStyleDeclaration = function CSSStyleDeclaration() {};

function datasetOf(el) {
  var s = el[DS];
  if (s) return s;
  function attr(p) { return 'data-' + String(p).replace(/[A-Z]/g, function(c) { return '-' + c.toLowerCase(); }); }
  s = new Proxy({}, {
    get: function(t, p) { if (typeof p === 'symbol') return undefined; var v = D.attr(el, attr(p)); return v === null ? undefined : v; },
    set: function(t, p, v) { D.setAttr(el, attr(p), String(v)); attrChanged(el, attr(p)); return true; },
    has: function(t, p) { return typeof p === 'string' && D.hasAttr(el, attr(p)); },
    deleteProperty: function(t, p) { D.removeAttr(el, attr(p)); return true; },
    ownKeys: function() {
      return D.attrs(el).map(function(a) { return a[0]; }).filter(function(n) { return n.indexOf('data-') === 0; })
        .map(function(n) { return n.slice(5).replace(/-([a-z])/g, function(m, c) { return c.toUpperCase(); }); });
    },
    getOwnPropertyDescriptor: function(t, p) {
      var v = D.attr(el, attr(p));
      return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
    }
  });
  Object.defineProperty(el, DS, { value: s, configurable: true });
  return s;
}

function Attr(el, name, value) { this.ownerElement = el; this.name = this.localName = this.nodeName = name; this._v = value; }
getters(Attr.prototype, {
  value: [function() { return this.ownerElement ? (D.attr(this.ownerElement, this.name) || '') : this._v; },
          function(v) { if (this.ownerElement) this.ownerElement.setAttribute(this.name, v); else this._v = String(v); }],
  nodeValue: function() { return this.value; },
  specified: function() { return true; }
});
Attr.prototype.nodeType = 2;
g.Attr = Attr;

function boundsOf(el) {
  return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0,
           toJSON: function() { return this; } };
}

getters(Element.prototype, {
  tagName: function() { return D.name(this); },
  localName: function() { return D.name(this).toLowerCase(); },
  namespaceURI: function() { return 'http://www.w3.org/1999/xhtml'; },
  prefix: function() { return null; },
  id: [function() { return D.attr(this, 'id') || ''; }, attrSetter('id')],
  className: [function() { return D.attr(this, 'class') || ''; }, attrSetter('class')],
  classList: [function() {
    return this[CL] || (Object.defineProperty(this, CL, { value: new DOMTokenList(this, 'class') }), this[CL]);
  }, function(v) { D.setAttr(this, 'class', String(v)); }],
  attributes: function() {
    var el = this;
    var a = D.attrs(this).map(function(p) { return new Attr(el, p[0], p[1]); });
    a.getNamedItem = function(n) { n = String(n).toLowerCase(); for (var i = 0; i < a.length; i++) if (a[i].name === n) return a[i]; return null; };
    a.item = function(i) { return a[i] || null; };
    a.removeNamedItem = function(n) { el.removeAttribute(n); };
    a.setNamedItem = function(at) { el.setAttribute(at.name, at.value); };
    return a;
  },
  innerHTML: [function() { return D.html(this, false); },
              function(v) {
                var old = mutationHook ? D.kids(this) : null;
                var f = D.parse(v == null ? '' : String(v)), c;
                while ((c = D.first(this))) D.remove(this, c);
                var nodes = D.kids(f);
                D.insert(this, f, null);
                for (var i = 0; i < nodes.length; i++) scriptsIn(nodes[i], true);
                if (isConnected(this)) ceConnected(nodes, true);
                else nodes.forEach(ceUpgradeIn);
                if (mutationHook) mutationHook('childList', this, nodes, old);
              }],
  outerHTML: [function() { return D.html(this, true); },
              function(v) {
                var p = D.parent(this);
                if (!p) return;
                var f = D.parse(String(v)), nodes = D.kids(f);
                D.insert(p, f, this);
                D.remove(p, this);
                if (mutationHook) mutationHook('childList', p, nodes, [this]);
              }],
  innerText: [function() { return D.text(this); }, function(v) { this.textContent = v; }],
  outerText: function() { return D.text(this); },
  nextElementSibling: function() { return nextEl(this); },
  previousElementSibling: function() { return prevEl(this); },
  style: [function() { return styleOf(this); }, function(v) { D.setAttr(this, 'style', String(v)); attrChanged(this, 'style'); }],
  dataset: function() { return datasetOf(this); },
  slot: function() { return ''; },
  shadowRoot: function() { return null; },
  assignedSlot: function() { return null; },
  clientWidth: function() { return 0; }, clientHeight: function() { return 0; },
  clientTop: function() { return 0; }, clientLeft: function() { return 0; },
  scrollWidth: function() { return 0; }, scrollHeight: function() { return 0; },
  scrollTop: [function() { return 0; }, function() {}],
  scrollLeft: [function() { return 0; }, function() {}]
});
methods(Element.prototype, {
  getAttribute: function(n) { return D.attr(this, String(n)); },
  getAttributeNS: function(ns_, n) { return D.attr(this, String(n)); },
  setAttribute: function(n, v) {
    n = String(n);
    if (!/^[^\s"'>\/=]+$/.test(n)) throw new TypeError("InvalidCharacterError: '" + n + "'");
    var old = this[CE] ? D.attr(this, n) : undefined;
    D.setAttr(this, n, String(v));
    attrChanged(this, n.toLowerCase(), old);
  },
  setAttributeNS: function(ns_, n, v) { this.setAttribute(String(n).replace(/^.*:/, ''), v); },
  removeAttribute: function(n) {
    var old = this[CE] ? D.attr(this, String(n)) : undefined;
    D.removeAttr(this, String(n));
    attrChanged(this, String(n).toLowerCase(), old);
  },
  removeAttributeNS: function(ns_, n) { this.removeAttribute(n); },
  hasAttribute: function(n) { return D.hasAttr(this, String(n)); },
  hasAttributeNS: function(ns_, n) { return D.hasAttr(this, String(n)); },
  hasAttributes: function() { return D.attrs(this).length > 0; },
  getAttributeNames: function() { return D.attrs(this).map(function(a) { return a[0]; }); },
  getAttributeNode: function(n) { var v = D.attr(this, String(n)); return v === null ? null : new Attr(this, String(n).toLowerCase(), v); },
  setAttributeNode: function(a) { this.setAttribute(a.name, a.value); return null; },
  toggleAttribute: function(n, force) {
    var has = this.hasAttribute(n);
    if (force === undefined ? has : !force) { if (has) this.removeAttribute(n); return false; }
    if (!has) this.setAttribute(n, '');
    return true;
  },
  matches: function(s) { return D.matches(this, String(s)); },
  webkitMatchesSelector: function(s) { return D.matches(this, String(s)); },
  msMatchesSelector: function(s) { return D.matches(this, String(s)); },
  closest: function(s) {
    s = String(s);
    for (var n = this; n && D.type(n) === 1; n = D.parent(n))
      if (D.matches(n, s)) return n;
    return null;
  },
  remove: function() { var p = D.parent(this); if (p) p.removeChild(this); },
  before: CharacterData.prototype.before,
  after: CharacterData.prototype.after,
  replaceWith: CharacterData.prototype.replaceWith,
  insertAdjacentElement: function(where, el) {
    var p = D.parent(this);
    switch (String(where).toLowerCase()) {
    case 'beforebegin': if (p) p.insertBefore(el, this); break;
    case 'afterbegin': this.insertBefore(el, D.first(this)); break;
    case 'beforeend': this.appendChild(el); break;
    case 'afterend': if (p) p.insertBefore(el, D.next(this)); break;
    default: throw new TypeError('SyntaxError: ' + where);
    }
    return el;
  },
  insertAdjacentHTML: function(where, html) {
    this.insertAdjacentElement(where, D.parse(String(html)));
  },
  insertAdjacentText: function(where, text) {
    this.insertAdjacentElement(where, D.createText(String(text)));
  },
  getBoundingClientRect: function() { return boundsOf(this); },
  getClientRects: function() { return []; },
  scrollIntoView: function() {}, scrollTo: function() {}, scrollBy: function() {}, scroll: function() {},
  focus: function() { dispatch(this, new g.FocusEvent('focus')); },
  blur: function() { dispatch(this, new g.FocusEvent('blur')); },
  // No shadow trees: a component renders into the element itself, so what
  // it draws is in the document (its scoped styles aside).
  attachShadow: function(init) {
    var el = this;
    if (!Object.getOwnPropertyDescriptor(el, 'shadowRoot'))
      Object.defineProperty(el, 'shadowRoot', { value: (init && init.mode === 'closed') ? null : el, configurable: true });
    if (!('host' in el)) Object.defineProperty(el, 'host', { value: el, configurable: true });
    if (!('adoptedStyleSheets' in el)) el.adoptedStyleSheets = [];
    if (!('mode' in el)) Object.defineProperty(el, 'mode', { value: (init && init.mode) || 'open', configurable: true });
    return el;
  },
  animate: function() { return { finished: Promise.resolve(), cancel: function() {}, play: function() {}, pause: function() {}, onfinish: null }; },
  getAnimations: function() { return []; },
  requestFullscreen: function() { return Promise.reject(new Error('NotSupportedError')); },
  setPointerCapture: function() {}, releasePointerCapture: function() {},
  hasPointerCapture: function() { return false; }
});

// HTMLElement's constructor is how custom elements come to be: run from a
// class's constructor (super()), it hands back the element being upgraded
// (customElements below), or makes one for `new MyElement()`.
var upgrading = null;
var HTMLElement = klass('HTMLElement', Element, function HTMLElement() {
  var t = upgrading, C = new.target;
  upgrading = null;
  if (!C || C === HTMLElement) throw new TypeError('Illegal constructor');
  if (!t) {
    var name = ceNames.get(C);
    if (!name) throw new TypeError('Illegal constructor: ' + (C.name || 'class') + ' is not a defined custom element');
    t = D.createElement(name);
  }
  Object.setPrototypeOf(t, C.prototype);
  return t;
});
reflect(HTMLElement.prototype, {
  title: 'title', lang: 'lang', dir: 'dir', accessKey: 'accesskey',
  hidden: ['bool', 'hidden'], draggable: ['bool', 'draggable'],
  tabIndex: ['num', 'tabindex', -1], contentEditable: 'contenteditable',
  translate: ['bool', 'translate'], spellcheck: ['bool', 'spellcheck'],
  nonce: 'nonce', inputMode: 'inputmode', enterKeyHint: 'enterkeyhint'
});
getters(HTMLElement.prototype, {
  offsetWidth: function() { return 0; }, offsetHeight: function() { return 0; },
  offsetTop: function() { return 0; }, offsetLeft: function() { return 0; },
  offsetParent: function() { return null; },
  isContentEditable: function() { return false; }
});
methods(HTMLElement.prototype, {
  click: function() {
    var ok = dispatch(this, new MouseEvent('click', { bubbles: true, cancelable: true }));
    if (ok) defaultAction(this, 'click');
  }
});
// the on... properties browsers have (null until set)
['abort', 'blur', 'change', 'click', 'contextmenu', 'dblclick', 'error', 'focus', 'input',
 'keydown', 'keypress', 'keyup', 'load', 'mousedown', 'mouseenter', 'mouseleave',
 'mousemove', 'mouseout', 'mouseover', 'mouseup', 'reset', 'resize', 'scroll', 'select',
 'submit', 'wheel', 'touchstart', 'touchend', 'touchmove', 'pointerdown', 'pointerup',
 'animationend', 'transitionend'].forEach(function(t) {
  var name = 'on' + t;
  Object.defineProperty(HTMLElement.prototype, name, {
    get: function() { return handlerOf(this, t); },
    set: function(f) { Object.defineProperty(this, name, { value: typeof f === 'function' ? f : null, writable: true, configurable: true }); },
    configurable: true
  });
});

// the element classes, and their tag names
var tagClasses = {};
function html(name, tags, props, extra) {
  var C = klass(name, HTMLElement);
  if (props) reflect(C.prototype, props);
  if (extra) {
    // 'get x': an accessor (a getter, or [getter, setter]); others: methods
    var gs = {}, ms = {};
    Object.keys(extra).forEach(function(k) {
      if (k.indexOf('get ') === 0) gs[k.slice(4)] = extra[k];
      else ms[k] = extra[k];
    });
    getters(C.prototype, gs);
    methods(C.prototype, ms);
  }
  tags.split(' ').forEach(function(t) { if (t) tagClasses[t.toUpperCase()] = C.prototype; });
  return C;
}
function urlParts(get) {
  var o = {};
  ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin', 'username', 'password'].forEach(function(k) {
    o['get ' + k] = function() { try { return new URLc(get(this))[k]; } catch (e) { return ''; } };
  });
  return o;
}
function assign(a, b) { for (var k in b) a[k] = b[k]; return a; }

html('HTMLAnchorElement', 'a', { href: ['url', 'href'], target: 'target', rel: 'rel',
  download: 'download', hreflang: 'hreflang', type: 'type', name: 'name', ping: 'ping',
  referrerPolicy: 'referrerpolicy' },
  assign(urlParts(function(el) { return el.href; }), {
    'get text': [function() { return D.text(this); }, function(v) { this.textContent = v; }],
    'get relList': function() { return new DOMTokenList(this, 'rel'); },
    toString: function() { return this.href; }
  }));
html('HTMLAreaElement', 'area', { href: ['url', 'href'], target: 'target', alt: 'alt',
  coords: 'coords', shape: 'shape', rel: 'rel' }, urlParts(function(el) { return el.href; }));
html('HTMLImageElement', 'img image', { src: ['url', 'src'], alt: 'alt', srcset: 'srcset',
  sizes: 'sizes', useMap: 'usemap', isMap: ['bool', 'ismap'], crossOrigin: 'crossorigin',
  loading: 'loading', decoding: 'decoding', referrerPolicy: 'referrerpolicy', name: 'name',
  align: 'align', border: 'border', vspace: ['num', 'vspace'], hspace: ['num', 'hspace'] }, {
  'get width': [function() { return parseInt(D.attr(this, 'width'), 10) || 0; }, function(v) { D.setAttr(this, 'width', String(v)); }],
  'get height': [function() { return parseInt(D.attr(this, 'height'), 10) || 0; }, function(v) { D.setAttr(this, 'height', String(v)); }],
  'get naturalWidth': function() { return parseInt(D.attr(this, 'width'), 10) || 0; },
  'get naturalHeight': function() { return parseInt(D.attr(this, 'height'), 10) || 0; },
  'get complete': function() { return true; },
  'get currentSrc': function() { return this.src; },
  decode: function() { return Promise.resolve(); }
});
var HTMLFormElement = html('HTMLFormElement', 'form', { action: ['url', 'action'],
  method: 'method', target: 'target', name: 'name', enctype: 'enctype',
  acceptCharset: 'accept-charset', autocomplete: 'autocomplete', noValidate: ['bool', 'novalidate'] }, {
  'get elements': function() { return list(D.select(this, 'input,select,textarea,button,fieldset,output,object', false)); },
  'get length': function() { return this.elements.length; },
  submit: function() { submitForm(this); },
  requestSubmit: function() {
    if (dispatch(this, new Event('submit', { bubbles: true, cancelable: true }))) submitForm(this);
  },
  reset: function() { dispatch(this, new Event('reset', { bubbles: true, cancelable: true })); },
  checkValidity: function() { return true; },
  reportValidity: function() { return true; }
});
var formControl = {
  'get form': function() {
    var f = D.attr(this, 'form');
    if (f) return document.getElementById(f);
    return this.closest('form');
  },
  'get labels': function() { return list([]); },
  'get validity': function() { return { valid: true, valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false, customError: false }; },
  'get validationMessage': function() { return ''; },
  'get willValidate': function() { return true; },
  checkValidity: function() { return true; },
  reportValidity: function() { return true; },
  setCustomValidity: function() {}
};
function liveValue(el) {
  var v = el[VAL];
  return v !== undefined ? v : null;
}
html('HTMLInputElement', 'input', { name: 'name', type: ['str', 'type'], defaultValue: 'value',
  placeholder: 'placeholder', disabled: ['bool', 'disabled'], readOnly: ['bool', 'readonly'],
  required: ['bool', 'required'], multiple: ['bool', 'multiple'], autofocus: ['bool', 'autofocus'],
  defaultChecked: ['bool', 'checked'], src: ['url', 'src'], alt: 'alt', accept: 'accept',
  autocomplete: 'autocomplete', min: 'min', max: 'max', step: 'step', pattern: 'pattern',
  size: ['num', 'size', 20], maxLength: ['num', 'maxlength', -1], minLength: ['num', 'minlength', -1],
  formAction: 'formaction', formMethod: 'formmethod', list: 'list', dirName: 'dirname' },
  assign(assign({}, formControl), {
  'get type': [function() { return (D.attr(this, 'type') || 'text').toLowerCase(); }, attrSetter('type')],
  'get value': [function() {
      if (!/^(checkbox|radio)$/.test(this.type)) {
        var f = D.field(this);
        if (f && f.value !== undefined) return f.value;
      }
      var v = liveValue(this);
      return v !== null ? v : (D.attr(this, 'value') || (/^(checkbox|radio)$/.test(this.type) ? 'on' : ''));
    },
    function(v) {
      v = v == null ? '' : String(v);
      if (!D.setField(this, 'value', v))
        Object.defineProperty(this, VAL, { value: v, writable: true, configurable: true });
    }],
  'get checked': [function() {
      var f = D.field(this);
      if (f && f.checked !== undefined) return f.checked;
      var c = this._checked; return c !== undefined ? c : D.hasAttr(this, 'checked');
    },
    function(v) { this._checked = !!v; D.setField(this, 'checked', !!v); }],
  'get valueAsNumber': function() { return parseFloat(this.value); },
  'get files': function() { return list([]); },
  'get selectionStart': function() { return this.value.length; },
  'get selectionEnd': function() { return this.value.length; },
  select: function() {}, setSelectionRange: function() {}, setRangeText: function() {}
}));
html('HTMLButtonElement', 'button', { name: 'name', value: 'value', disabled: ['bool', 'disabled'],
  autofocus: ['bool', 'autofocus'], formAction: 'formaction' }, assign(assign({}, formControl), {
  'get type': [function() { return (D.attr(this, 'type') || 'submit').toLowerCase(); }, attrSetter('type')]
}));
html('HTMLSelectElement', 'select', { name: 'name', disabled: ['bool', 'disabled'],
  multiple: ['bool', 'multiple'], required: ['bool', 'required'], size: ['num', 'size'] },
  assign(assign({}, formControl), {
  'get options': function() { var o = list(D.select(this, 'option', false)); var s = this; o.add = function(e, b) { s.add(e, b); }; o.remove = function(i) { s.remove(i); }; return o; },
  'get length': function() { return D.select(this, 'option', false).length; },
  'get type': function() { return this.multiple ? 'select-multiple' : 'select-one'; },
  'get selectedIndex': [function() {
      var f = D.field(this), i;
      if (f && f.selected) { for (i = 0; i < f.selected.length; i++) if (f.selected[i]) return i; return -1; }
      var o = D.select(this, 'option', false);
      for (i = 0; i < o.length; i++) if (o[i].selected) return i;
      return o.length && !this.multiple ? 0 : -1;
    }, function(n) {
      var o = D.select(this, 'option', false);
      for (var i = 0; i < o.length; i++) o[i]._sel = i === n;
      D.setField(this, 'selected', n | 0);
    }],
  'get selectedOptions': function() { return list(D.select(this, 'option', false).filter(function(o) { return o.selected; })); },
  'get value': [function() { var o = this.options[this.selectedIndex]; return o ? o.value : ''; },
                function(v) { var o = D.select(this, 'option', false); for (var i = 0; i < o.length; i++) o[i].selected = o[i].value === String(v); }],
  add: function(el, before) { this.insertBefore(el, typeof before === 'number' ? this.options[before] : before || null); },
  item: function(i) { return this.options[i] || null; }
}));
html('HTMLOptionElement', 'option', { label: 'label', disabled: ['bool', 'disabled'],
  defaultSelected: ['bool', 'selected'] }, {
  'get value': [function() { var v = D.attr(this, 'value'); return v === null ? D.text(this).trim() : v; }, attrSetter('value')],
  'get text': [function() { return D.text(this); }, function(v) { this.textContent = v; }],
  'get selected': [function() {
      var s = this.closest('select'), f = s && D.field(s);
      if (f && f.selected) { var i = D.select(s, 'option', false).indexOf(this); if (i >= 0) return !!f.selected[i]; }
      return this._sel !== undefined ? this._sel : D.hasAttr(this, 'selected');
    }, function(v) {
      this._sel = !!v;
      var s = this.closest('select');
      if (s && v) D.setField(s, 'selected', D.select(s, 'option', false).indexOf(this));
    }],
  'get index': function() { var s = this.closest('select'); return s ? D.select(s, 'option', false).indexOf(this) : 0; },
  'get form': function() { return this.closest('form'); }
});
g.Option = function Option(text, value, dflt, sel) {
  var o = document.createElement('option');
  if (text !== undefined) o.textContent = text;
  if (value !== undefined) o.setAttribute('value', value);
  if (dflt) o.setAttribute('selected', '');
  if (sel) o.selected = true;
  return o;
};
html('HTMLOptGroupElement', 'optgroup', { label: 'label', disabled: ['bool', 'disabled'] });
html('HTMLTextAreaElement', 'textarea', { name: 'name', placeholder: 'placeholder',
  disabled: ['bool', 'disabled'], readOnly: ['bool', 'readonly'], required: ['bool', 'required'],
  rows: ['num', 'rows', 2], cols: ['num', 'cols', 20], maxLength: ['num', 'maxlength', -1],
  wrap: 'wrap', autocomplete: 'autocomplete' }, assign(assign({}, formControl), {
  'get type': function() { return 'textarea'; },
  'get value': [function() {
      var f = D.field(this);
      if (f && f.value !== undefined) return f.value;
      var v = liveValue(this); return v !== null ? v : D.text(this);
    },
    function(v) {
      v = v == null ? '' : String(v);
      if (!D.setField(this, 'value', v))
        Object.defineProperty(this, VAL, { value: v, writable: true, configurable: true });
    }],
  'get defaultValue': [function() { return D.text(this); }, function(v) { this.textContent = v; }],
  select: function() {}, setSelectionRange: function() {}
}));
html('HTMLLabelElement', 'label', { htmlFor: 'for' }, {
  'get control': function() { var f = D.attr(this, 'for'); return f ? document.getElementById(f) : this.querySelector('input,select,textarea,button'); },
  'get form': function() { return this.closest('form'); }
});
html('HTMLFieldSetElement', 'fieldset', { name: 'name', disabled: ['bool', 'disabled'] }, formControl);
html('HTMLLegendElement', 'legend');
html('HTMLOutputElement', 'output', { name: 'name' }, formControl);
html('HTMLScriptElement', 'script', { src: ['url', 'src'], type: 'type', charset: 'charset',
  async: ['bool', 'async'], defer: ['bool', 'defer'], crossOrigin: 'crossorigin',
  integrity: 'integrity', noModule: ['bool', 'nomodule'], referrerPolicy: 'referrerpolicy',
  nonce: 'nonce', event: 'event', htmlFor: 'for' }, {
  'get text': [function() { return D.text(this); }, function(v) { this.textContent = v; }]
});
g.HTMLScriptElement.supports = function(t) { return t === 'classic'; };
html('HTMLStyleElement', 'style', { media: 'media', type: 'type' }, {
  'get sheet': function() { return { cssRules: [], rules: [], insertRule: function() { return 0; }, deleteRule: function() {}, disabled: false }; },
  'get disabled': [function() { return false; }, function() {}]
});
html('HTMLLinkElement', 'link', { href: ['url', 'href'], rel: 'rel', media: 'media',
  type: 'type', as: 'as', crossOrigin: 'crossorigin', hreflang: 'hreflang',
  integrity: 'integrity', sizes: 'sizes', disabled: ['bool', 'disabled'] }, {
  'get relList': function() { return new DOMTokenList(this, 'rel'); },
  'get sheet': function() { return null; }
});
html('HTMLMetaElement', 'meta', { name: 'name', content: 'content', httpEquiv: 'http-equiv', charset: 'charset' });
html('HTMLBaseElement', 'base', { href: ['url', 'href'], target: 'target' });
html('HTMLTitleElement', 'title', null, {
  'get text': [function() { return D.text(this); }, function(v) { this.textContent = v; }]
});
html('HTMLHeadElement', 'head');
html('HTMLHtmlElement', 'html', { version: 'version' });
html('HTMLBodyElement', 'body', { bgColor: 'bgcolor', background: 'background', text: 'text',
  link: 'link', vLink: 'vlink', aLink: 'alink' });
html('HTMLDivElement', 'div', { align: 'align' });
html('HTMLSpanElement', 'span');
html('HTMLParagraphElement', 'p', { align: 'align' });
html('HTMLHeadingElement', 'h1 h2 h3 h4 h5 h6', { align: 'align' });
html('HTMLBRElement', 'br', { clear: 'clear' });
html('HTMLHRElement', 'hr', { align: 'align', noShade: ['bool', 'noshade'], size: 'size', width: 'width' });
html('HTMLPreElement', 'pre listing xmp', { width: ['num', 'width'] });
html('HTMLQuoteElement', 'blockquote q', { cite: ['url', 'cite'] });
html('HTMLUListElement', 'ul', { compact: ['bool', 'compact'], type: 'type' });
html('HTMLOListElement', 'ol', { reversed: ['bool', 'reversed'], start: ['num', 'start', 1], type: 'type' });
html('HTMLLIElement', 'li', { value: ['num', 'value'], type: 'type' });
html('HTMLDListElement', 'dl');
html('HTMLMenuElement', 'menu');
html('HTMLDirectoryElement', 'dir');
html('HTMLModElement', 'ins del', { cite: ['url', 'cite'], dateTime: 'datetime' });
html('HTMLTimeElement', 'time', { dateTime: 'datetime' });
html('HTMLDataElement', 'data', { value: 'value' });
html('HTMLFontElement', 'font', { color: 'color', face: 'face', size: 'size' });
html('HTMLTableElement', 'table', { border: 'border', cellPadding: 'cellpadding',
  cellSpacing: 'cellspacing', width: 'width', align: 'align', bgColor: 'bgcolor',
  summary: 'summary', frame: 'frame', rules: 'rules' }, {
  'get rows': function() { return list(D.select(this, ':scope > tr, :scope > tbody > tr, :scope > thead > tr, :scope > tfoot > tr', false)); },
  'get tBodies': function() { return list(D.select(this, ':scope > tbody', false)); },
  'get tHead': function() { return D.select(this, ':scope > thead', true); },
  'get tFoot': function() { return D.select(this, ':scope > tfoot', true); },
  'get caption': function() { return D.select(this, ':scope > caption', true); },
  insertRow: function(i) {
    var b = this.tBodies[0] || this.appendChild(document.createElement('tbody'));
    var r = document.createElement('tr'), rows = this.rows;
    if (i === undefined || i < 0 || i >= rows.length) b.appendChild(r);
    else D.parent(rows[i]).insertBefore(r, rows[i]);
    return r;
  },
  deleteRow: function(i) { var r = this.rows[i < 0 ? this.rows.length - 1 : i]; if (r) r.remove(); },
  createTBody: function() { return this.appendChild(document.createElement('tbody')); },
  createTHead: function() { return this.tHead || this.insertBefore(document.createElement('thead'), D.first(this)); },
  createTFoot: function() { return this.tFoot || this.appendChild(document.createElement('tfoot')); },
  createCaption: function() { return this.caption || this.insertBefore(document.createElement('caption'), D.first(this)); }
});
html('HTMLTableSectionElement', 'tbody thead tfoot', { align: 'align', vAlign: 'valign' }, {
  'get rows': function() { return list(D.select(this, ':scope > tr', false)); },
  insertRow: function(i) {
    var r = document.createElement('tr'), rows = this.rows;
    if (i === undefined || i < 0 || i >= rows.length) this.appendChild(r); else this.insertBefore(r, rows[i]);
    return r;
  },
  deleteRow: function(i) { var r = this.rows[i]; if (r) r.remove(); }
});
html('HTMLTableRowElement', 'tr', { align: 'align', vAlign: 'valign', bgColor: 'bgcolor' }, {
  'get cells': function() { return list(D.select(this, ':scope > td, :scope > th', false)); },
  'get rowIndex': function() { var t = this.closest('table'); return t ? t.rows.indexOf(this) : -1; },
  'get sectionRowIndex': function() { var p = D.parent(this); return p ? D.select(p, ':scope > tr', false).indexOf(this) : -1; },
  insertCell: function(i) {
    var c = document.createElement('td'), cells = this.cells;
    if (i === undefined || i < 0 || i >= cells.length) this.appendChild(c); else this.insertBefore(c, cells[i]);
    return c;
  },
  deleteCell: function(i) { var c = this.cells[i]; if (c) c.remove(); }
});
html('HTMLTableCellElement', 'td th', { colSpan: ['num', 'colspan', 1], rowSpan: ['num', 'rowspan', 1],
  headers: 'headers', align: 'align', vAlign: 'valign', bgColor: 'bgcolor', width: 'width',
  height: 'height', noWrap: ['bool', 'nowrap'], abbr: 'abbr', scope: 'scope' }, {
  'get cellIndex': function() { var p = D.parent(this); return p ? D.select(p, ':scope > td, :scope > th', false).indexOf(this) : -1; }
});
html('HTMLTableCaptionElement', 'caption', { align: 'align' });
html('HTMLTableColElement', 'col colgroup', { span: ['num', 'span', 1], width: 'width' });
html('HTMLIFrameElement', 'iframe', { src: ['url', 'src'], name: 'name', width: 'width',
  height: 'height', allow: 'allow', loading: 'loading', srcdoc: 'srcdoc', referrerPolicy: 'referrerpolicy' }, {
  'get contentWindow': function() { return null; },
  'get contentDocument': function() { return null; }
});
html('HTMLFrameElement', 'frame', { src: ['url', 'src'], name: 'name' });
html('HTMLFrameSetElement', 'frameset', { rows: 'rows', cols: 'cols' });
html('HTMLObjectElement', 'object', { data: ['url', 'data'], type: 'type', name: 'name', width: 'width', height: 'height' });
html('HTMLEmbedElement', 'embed', { src: ['url', 'src'], type: 'type', width: 'width', height: 'height' });
html('HTMLParamElement', 'param', { name: 'name', value: 'value' });
html('HTMLMapElement', 'map', { name: 'name' }, { 'get areas': function() { return list(D.select(this, 'area', false)); } });
html('HTMLCanvasElement', 'canvas', { width: ['num', 'width', 300], height: ['num', 'height', 150] }, {
  getContext: function() { return null; },
  toDataURL: function() { return 'data:,'; },
  toBlob: function(cb) { setTimeout(function() { cb(null); }, 0); }
});
var mediaProps = { src: ['url', 'src'], autoplay: ['bool', 'autoplay'], controls: ['bool', 'controls'],
  loop: ['bool', 'loop'], muted: ['bool', 'muted'], preload: 'preload', poster: ['url', 'poster'] };
var mediaMethods = {
  play: function() { return Promise.reject(new Error('NotSupportedError: no media playback')); },
  pause: function() {}, load: function() {},
  canPlayType: function() { return ''; },
  'get paused': function() { return true; }, 'get ended': function() { return false; },
  'get currentTime': [function() { return 0; }, function() {}],
  'get duration': function() { return NaN; }, 'get readyState': function() { return 0; },
  'get volume': [function() { return 1; }, function() {}]
};
html('HTMLMediaElement', '', mediaProps, mediaMethods);
html('HTMLVideoElement', 'video', mediaProps, mediaMethods);
html('HTMLAudioElement', 'audio', mediaProps, mediaMethods);
g.Audio = function Audio(src) { var a = document.createElement('audio'); if (src) a.setAttribute('src', src); return a; };
html('HTMLSourceElement', 'source', { src: ['url', 'src'], type: 'type', srcset: 'srcset', media: 'media', sizes: 'sizes' });
html('HTMLTrackElement', 'track', { src: ['url', 'src'], kind: 'kind', label: 'label', srclang: 'srclang' });
html('HTMLPictureElement', 'picture');
html('HTMLTemplateElement', 'template', null, {
  'get content': function() {
    if (!this._content) {
      var f = D.createFragment(), c;
      while ((c = D.first(this))) D.insert(f, c, null);
      Object.defineProperty(this, '_content', { value: f });
    }
    return this._content;
  }
});
html('HTMLSlotElement', 'slot', { name: 'name' }, { assignedNodes: function() { return []; }, assignedElements: function() { return []; } });
html('HTMLDetailsElement', 'details', { open: ['bool', 'open'] });
html('HTMLDialogElement', 'dialog', { open: ['bool', 'open'] }, {
  show: function() { D.setAttr(this, 'open', ''); }, showModal: function() { D.setAttr(this, 'open', ''); },
  close: function(v) { D.removeAttr(this, 'open'); this.returnValue = v || ''; dispatch(this, new Event('close')); }
});
html('HTMLProgressElement', 'progress', { max: ['num', 'max', 1] }, {
  'get value': [function() { return parseFloat(D.attr(this, 'value')) || 0; }, function(v) { D.setAttr(this, 'value', String(v)); }]
});
html('HTMLMeterElement', 'meter', { min: 'min', max: 'max', low: 'low', high: 'high', optimum: 'optimum', value: 'value' });
html('HTMLUnknownElement', '');
html('HTMLMarqueeElement', 'marquee');
html('HTMLBlinkElement', 'blink');
// SVG elements get here too (the document is HTML): enough for scripts
// that look at them
var SVGElement = klass('SVGElement', Element);
klass('SVGSVGElement', SVGElement);
klass('SVGGraphicsElement', SVGElement);
tagClasses.SVG = g.SVGSVGElement.prototype;
['path', 'g', 'circle', 'rect', 'line', 'polyline', 'polygon', 'use', 'symbol', 'defs', 'text',
 'tspan', 'ellipse', 'image', 'lineargradient', 'radialgradient', 'stop', 'clippath', 'mask'].forEach(function(t) {
  if (t !== 'image') tagClasses[t.toUpperCase()] = g.SVGGraphicsElement.prototype;
});

// ---- the document ---------------------------------------------------------------

var Document = klass('Document', Node);
var HTMLDocument = klass('HTMLDocument', Document);
getters(Document.prototype, parentNodeMixin);
methods(Document.prototype, parentNodeMethods);

function headOrBody(tag) {
  var r = D.first(document);
  while (r && D.type(r) !== 1) r = D.next(r);
  if (!r) return null;
  tag = tag.toUpperCase();
  for (var n = D.first(r); n; n = D.next(n))
    if (D.type(n) === 1 && D.name(n) === tag) return n;
  return null;
}
var readyState = 'loading';
getters(Document.prototype, {
  documentElement: function() { var n = D.first(this); while (n && D.type(n) !== 1) n = D.next(n); return n; },
  head: function() { return headOrBody('head'); },
  body: [function() { return headOrBody('body') || headOrBody('frameset'); },
         function(b) { var old = this.body, r = this.documentElement; if (old) r.replaceChild(b, old); else r.appendChild(b); }],
  title: [function() { var t = D.select(this, 'title', true); return t ? D.text(t).replace(/\s+/g, ' ').trim() : ns.title(); },
          function(v) {
            var t = D.select(this, 'title', true);
            if (!t) { t = this.createElement('title'); var h = this.head; if (h) h.appendChild(t); }
            t.textContent = String(v);
          }],
  URL: function() { return ns.url(); },
  documentURI: function() { return ns.url(); },
  baseURI: function() { var b = D.select(this, 'base[href]', true); return b ? b.href : ns.url(); },
  referrer: function() { return ns.referrer(); },
  cookie: [function() { return ns.cookie(); }, function(c) { ns.setCookie(String(c)); }],
  domain: [function() { return g.location.hostname; }, function() {}],
  location: [function() { return g.location; }, function(u) { g.location.href = u; }],
  readyState: function() { return readyState; },
  characterSet: function() { return 'UTF-8'; },
  charset: function() { return 'UTF-8'; },
  inputEncoding: function() { return 'UTF-8'; },
  contentType: function() { return 'text/html'; },
  compatMode: function() { return 'CSS1Compat'; },
  doctype: function() { return null; },
  defaultView: function() { return g; },
  hidden: function() { return false; },
  visibilityState: function() { return 'visible'; },
  activeElement: function() { return this.body; },
  fullscreenElement: function() { return null; },
  fullscreenEnabled: function() { return false; },
  pointerLockElement: function() { return null; },
  scrollingElement: function() { return this.documentElement; },
  currentScript: function() { return currentScript !== undefined ? currentScript : D.currentScript(); },
  forms: function() { return list(D.byTag(this, 'form')); },
  images: function() { return list(D.byTag(this, 'img')); },
  embeds: function() { return list(D.byTag(this, 'embed')); },
  plugins: function() { return list(D.byTag(this, 'embed')); },
  scripts: function() { return list(D.byTag(this, 'script')); },
  links: function() { return list(D.select(this, 'a[href],area[href]', false)); },
  anchors: function() { return list(D.select(this, 'a[name]', false)); },
  styleSheets: function() { return list([]); },
  lastModified: function() { return new Date().toLocaleString(); },
  designMode: [function() { return 'off'; }, function() {}],
  dir: [function() { return ''; }, function() {}],
  fonts: function() { return { ready: Promise.resolve(), check: function() { return true; }, load: function() { return Promise.resolve([]); }, addEventListener: function() {}, status: 'loaded' }; },
  implementation: function() { return implementation; }
});
var currentScript;
methods(Document.prototype, {
  getElementById: function(id) { return D.byId(String(id)); },
  getElementsByName: function(n) { return list(D.select(this, '[name="' + String(n).replace(/["\\]/g, '\\$&') + '"]', false)); },
  createElement: function(n) {
    n = String(n);
    if (!/^[A-Za-z][^\s"'>\/=]*$/.test(n)) throw new TypeError("InvalidCharacterError: '" + n + "'");
    var el = D.createElement(n.toLowerCase());
    if (ceDefs[n.toLowerCase()]) ceUpgrade(el);
    return el;
  },
  createElementNS: function(ns_, n) { return D.createElement(String(n).replace(/^.*:/, '')); },
  createTextNode: function(s) { return D.createText(String(s)); },
  createComment: function(s) { return D.createComment(String(s)); },
  createDocumentFragment: function() { return D.createFragment(); },
  createAttribute: function(n) { return new Attr(null, String(n).toLowerCase(), ''); },
  createEvent: function(t) {
    var C = g[String(t).replace(/s$/, '')] || Event;
    var e = Object.create(C.prototype);
    Event.call(e, '');
    return e;
  },
  createRange: function() { return new Range(); },
  createTreeWalker: function(root, what, filter) { return new TreeWalker(root, what, filter); },
  createNodeIterator: function(root, what, filter) { return new TreeWalker(root, what, filter); },
  importNode: function(n, deep) { return D.clone(n, !!deep); },
  adoptNode: function(n) { var p = D.parent(n); if (p) D.remove(p, n); return n; },
  hasFocus: function() { return true; },
  elementFromPoint: function() { return null; },
  elementsFromPoint: function() { return []; },
  getSelection: function() { return g.getSelection(); },
  execCommand: function() { return false; },
  queryCommandSupported: function() { return false; },
  exitFullscreen: function() { return Promise.resolve(); },
  write: function() { ns.write([].join.call(arguments, '')); },
  writeln: function() { ns.write([].join.call(arguments, '') + '\n'); },
  open: function() { return this; },
  close: function() {}
});

// a document of its own for scripts that parse HTML into one (jQuery)
var implementation = {
  hasFeature: function() { return true; },
  createHTMLDocument: function(title) {
    var d = Object.create(Document.prototype), root = D.createElement('html'),
        head = D.createElement('head'), body = D.createElement('body');
    root.appendChild(head); root.appendChild(body);
    Object.defineProperties(d, {
      documentElement: { value: root }, head: { value: head }, body: { value: body },
      title: { value: title || '', writable: true },
      createElement: { value: function(n) { return document.createElement(n); } },
      getElementById: { value: function(id) { return D.select(root, '[id="' + id + '"]', true); } },
      querySelector: { value: function(s) { return D.select(root, s, true); } },
      querySelectorAll: { value: function(s) { return list(D.select(root, s, false)); } }
    });
    return d;
  },
  createDocument: function() { return this.createHTMLDocument(''); },
  createDocumentType: function() { return null; }
};

// ---- ranges, selections, tree walkers: enough to not break -------------------

function Range() { this.startContainer = this.endContainer = document; this.startOffset = this.endOffset = 0; this.collapsed = true; this.commonAncestorContainer = document; }
methods(Range.prototype, {
  setStart: function(n, o) { this.startContainer = n; this.startOffset = o; },
  setEnd: function(n, o) { this.endContainer = n; this.endOffset = o; },
  setStartBefore: function() {}, setStartAfter: function() {}, setEndBefore: function() {}, setEndAfter: function() {},
  selectNode: function(n) { this.startContainer = this.endContainer = n; },
  selectNodeContents: function(n) { this.startContainer = this.endContainer = n; },
  collapse: function() {}, cloneRange: function() { return new Range(); }, detach: function() {},
  deleteContents: function() {}, extractContents: function() { return D.createFragment(); },
  cloneContents: function() { return D.createFragment(); }, insertNode: function() {},
  surroundContents: function() {},
  createContextualFragment: function(h) { return D.parse(String(h)); },
  getBoundingClientRect: function() { return boundsOf(); }, getClientRects: function() { return []; },
  toString: function() { return ''; }
});
g.Range = Range;
g.getSelection = function() {
  return { rangeCount: 0, isCollapsed: true, anchorNode: null, focusNode: null, type: 'None',
    getRangeAt: function() { return new Range(); }, removeAllRanges: function() {}, addRange: function() {},
    collapse: function() {}, selectAllChildren: function() {}, toString: function() { return ''; } };
};
var NodeFilter = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF,
  SHOW_ELEMENT: 1, SHOW_TEXT: 4, SHOW_COMMENT: 128 };
g.NodeFilter = NodeFilter;
function TreeWalker(root, what, filter) {
  this.root = root; this.currentNode = root;
  this.whatToShow = what === undefined ? 0xFFFFFFFF : what; this.filter = filter || null;
}
methods(TreeWalker.prototype, {
  _ok: function(n) {
    if (!(this.whatToShow & (1 << (D.type(n) - 1)))) return 3;
    if (!this.filter) return 1;
    return typeof this.filter === 'function' ? this.filter(n) : this.filter.acceptNode(n);
  },
  _next: function(n) {
    var c = D.first(n);
    if (c) return c;
    while (n && n !== this.root) { var s = D.next(n); if (s) return s; n = D.parent(n); }
    return null;
  },
  nextNode: function() {
    for (var n = this._next(this.currentNode); n; n = this._next(n))
      if (this._ok(n) === 1) return (this.currentNode = n);
    return null;
  },
  previousNode: function() { return null; },
  parentNode: function() {
    for (var n = D.parent(this.currentNode); n && n !== this.root; n = D.parent(n))
      if (this._ok(n) === 1) return (this.currentNode = n);
    return null;
  },
  firstChild: function() { var n = D.first(this.currentNode); while (n && this._ok(n) !== 1) n = D.next(n); return n ? (this.currentNode = n) : null; },
  nextSibling: function() { var n = D.next(this.currentNode); while (n && this._ok(n) !== 1) n = D.next(n); return n ? (this.currentNode = n) : null; },
  lastChild: function() { return null; }, previousSibling: function() { return null; },
  detach: function() {}
});
g.TreeWalker = g.NodeIterator = TreeWalker;

// ---- MutationObserver: what scripts change through this DOM --------------

var observers = [];
var MutationObserver = klass('MutationObserver', null, function MutationObserver(cb) {
  this._cb = cb; this._targets = []; this._records = [];
});
methods(MutationObserver.prototype, {
  observe: function(target, opts) {
    opts = opts || {};
    if (opts.attributeFilter || opts.attributeOldValue) opts.attributes = true;
    this._targets.push({ node: target, opts: opts });
    if (observers.indexOf(this) < 0) observers.push(this);
    mutationHook = notify;
  },
  disconnect: function() {
    this._targets = [];
    var i = observers.indexOf(this);
    if (i >= 0) observers.splice(i, 1);
    if (!observers.length) mutationHook = null;
  },
  takeRecords: function() { var r = this._records; this._records = []; return r; }
});
g.WebKitMutationObserver = MutationObserver;
var notifyQueued = false;
function notify(type, target, added, removedNodes, attr) {
  observers.forEach(function(o) {
    o._targets.forEach(function(t) {
      var opts = t.opts;
      if (t.node !== target && !(opts.subtree && D.contains(t.node, target))) return;
      if (type === 'childList' && !opts.childList) return;
      if (type === 'attributes' && (!opts.attributes || (opts.attributeFilter && opts.attributeFilter.indexOf(attr) < 0))) return;
      o._records.push({ type: type, target: target, addedNodes: list(added || []),
        removedNodes: list(removedNodes || []), attributeName: attr || null,
        previousSibling: null, nextSibling: null, oldValue: null });
    });
  });
  if (!notifyQueued) {
    notifyQueued = true;
    Promise.resolve().then(function() {
      notifyQueued = false;
      observers.slice().forEach(function(o) {
        var r = o.takeRecords();
        if (r.length) try { o._cb(r, o); } catch (x) { report('MutationObserver', x); }
      });
    });
  }
}

// IntersectionObserver: everything is in view (so lazy content loads)
klass('IntersectionObserver', null, function IntersectionObserver(cb, opts) {
  this._cb = cb; this.root = (opts && opts.root) || null; this.rootMargin = '0px'; this.thresholds = [0];
});
methods(g.IntersectionObserver.prototype, {
  observe: function(el) {
    var self = this;
    setTimeout(function() {
      try {
        self._cb([{ target: el, isIntersecting: true, intersectionRatio: 1, time: Date.now(),
          boundingClientRect: boundsOf(el), intersectionRect: boundsOf(el), rootBounds: null }], self);
      } catch (x) { report('IntersectionObserver', x); }
    }, 0);
  },
  unobserve: function() {}, disconnect: function() {}, takeRecords: function() { return []; }
});
klass('ResizeObserver', null, function ResizeObserver(cb) { this._cb = cb; });
methods(g.ResizeObserver.prototype, { observe: function() {}, unobserve: function() {}, disconnect: function() {} });
klass('PerformanceObserver', null, function PerformanceObserver() {});
methods(g.PerformanceObserver.prototype, { observe: function() {}, disconnect: function() {} });
g.PerformanceObserver.supportedEntryTypes = [];

// ---- scripts the page puts in, and fetching ----------------------------------

function isJS(type) {
  if (!type) return true;
  type = type.trim().toLowerCase();
  return type === '' || /^(text|application)\/(x-)?(java|ecma)script(1\.\d)?$/.test(type) ||
    type === 'text/jscript' || type === 'text/livescript';
}
// ---- ES modules: fetch the graph (qjs_window.c's loader serves it) ----------

var modsrc = Object.create(null);		// URL -> source
Object.defineProperty(g, '__ns_modsrc', { value: modsrc });
var importMap = Object.create(null);
g.__ns_importmap = function(name) {
  readImportMaps();
  if (importMap[name]) return importMap[name];
  var best = '';
  for (var k in importMap)
    if (k.charAt(k.length - 1) === '/' && name.indexOf(k) === 0 && k.length > best.length) best = k;
  return best ? importMap[best] + name.slice(best.length) : null;
};
var importMapsRead = 0;
function readImportMaps() {
  var maps = D.select(document, 'script[type="importmap"]', false);
  for (; importMapsRead < maps.length; importMapsRead++) {
    try {
      var j = JSON.parse(D.text(maps[importMapsRead]));
      for (var k in (j.imports || {})) importMap[k] = ns.resolve(j.imports[k]);
    } catch (x) { report('import map', x); }
  }
}
var importRe = /(?:^|[^.\w$])(?:import|export)\s*(?:[\w$*{}\s,]*?\bfrom\s*)?(["'])([^"'\n]+)\1/g;
var dynImportRe = /(?:^|[^.\w$])import\s*\(\s*(["'])([^"'\n]+)\1\s*\)/g;
function moduleURL(spec, base) {
  if (/^(\.{1,2}\/|\/)/.test(spec) || /^[a-z][a-z0-9+.-]*:/i.test(spec)) return ns.resolve(spec, base);
  return g.__ns_importmap(spec);
}
var modInflight = Object.create(null);	// URL -> callbacks for when it is in
var modFetched = 0;
// Fetch the module ENTRY (a URL) or the imports of SRC (inline, at BASE),
// and everything they import; then done().
function loadModuleGraph(entry, src, base, done) {
  var outstanding = 0, over = false;
  function check() { if (outstanding === 0 && !over) { over = true; done(); } }
  function scan(text, at) {
    var m, re = [importRe, dynImportRe];
    for (var r = 0; r < 2; r++) {
      re[r].lastIndex = 0;
      while ((m = re[r].exec(text))) {
        var u = moduleURL(m[2], at);
        if (u && !/\.(css|json|svg|png|jpe?g|gif|webp|wasm|html?)([?#]|$)/i.test(u)) want(u);
      }
    }
  }
  function want(url) {
    if (url in modsrc) return;
    outstanding++;
    if (modInflight[url]) { modInflight[url].push(function() { outstanding--; check(); }); return; }
    if (++modFetched > 500) { log('modules: more than 500 on one page'); outstanding--; return; }
    modInflight[url] = [];
    ns.load(url, 'GET', null, '', function(status, text) {
      var waiting = modInflight[url];
      delete modInflight[url];
      if (status >= 200 && status < 300) { modsrc[url] = text; scan(text, url); }
      else log('module ' + url + ': failed (' + status + ')');
      outstanding--;
      waiting.forEach(function(f) { f(); });
      check();
    });
  }
  if (src !== null) scan(src, base); else want(entry);
  check();
}
var inlineModules = 0;
// Run the module script S (fetching what it needs first); then done().
function runModule(s, done) {
  var src = D.attr(s, 'src');
  readImportMaps();
  function finish(ok) { dispatch(s, new Event(ok ? 'load' : 'error')); if (done) done(); }
  function evaluate(text, name) {
    var p;
    currentScript = null;		// null while a module runs
    try { p = ns.evalModule(text, name); }
    catch (x) { currentScript = undefined; report('module ' + name, x); return finish(false); }
    currentScript = undefined;
    if (p && typeof p.then === 'function')
      p.then(function() { finish(true); }, function(x) { report('module ' + name, x); finish(false); });
    else finish(true);
  }
  if (src !== null && src.trim() !== '') {
    var url = ns.resolve(src.trim());
    loadModuleGraph(url, null, url, function() {
      if (url in modsrc) evaluate(modsrc[url], url); else finish(false);
    });
  } else {
    var text = D.text(s), name = ns.url().replace(/#.*$/, '') + '#module' + (++inlineModules);
    loadModuleGraph(name, text, ns.url(), function() { evaluate(text, name); });
  }
}
// The page's module scripts, in order (they are deferred: after parsing,
// before DOMContentLoaded); then done().  A page with nomodule scripts has
// had those run instead.
function runPageModules(done) {
  var all = D.select(document, 'script[type="module"]', false).filter(function(s) { return !s[DONE]; });
  if (!all.length || D.select(document, 'script[nomodule]', true)) return done();
  all.forEach(function(s) { Object.defineProperty(s, DONE, { value: true }); });
  var i = 0, over = false;
  var timer = setTimeout(function() { if (!over) { over = true; log('modules: still loading after 30 s'); done(); } }, 30000);
  (function next() {
    if (over) return;
    if (i >= all.length) { over = true; clearTimeout(timer); return done(); }
    runModule(all[i++], next);
  })();
}

function runScript(s) {
  if (s[DONE]) return;
  Object.defineProperty(s, DONE, { value: true });
  var type = D.attr(s, 'type'), src = D.attr(s, 'src');
  if (type && type.trim().toLowerCase() === 'module') {
    runModule(s, null);
    return;
  }
  if (!isJS(type) || D.hasAttr(s, 'nomodule') && false) return;
  if (src !== null && src.trim() !== '') {
    var url = ns.resolve(src.trim());
    ns.load(url, 'GET', null, '', function(status, text) {
      if (status >= 200 && status < 300) {
        currentScript = s;
        ns.evalScript(text, url);
        currentScript = undefined;
        dispatch(s, new Event('load'));
      } else {
        log('script ' + url + ': failed (' + status + ')');
        dispatch(s, new Event('error'));
      }
    });
  } else {
    var text = D.text(s);
    if (text.trim()) {
      currentScript = s;
      ns.evalScript(text, ns.url());
      currentScript = undefined;
    }
  }
}

function Headers(init) {
  this._h = {};
  if (init) {
    if (init instanceof Headers) init.forEach(function(v, k) { this.set(k, v); }, this);
    else if (Array.isArray(init)) init.forEach(function(p) { this.append(p[0], p[1]); }, this);
    else for (var k in init) this.append(k, init[k]);
  }
}
methods(Headers.prototype, {
  get: function(k) { var v = this._h[String(k).toLowerCase()]; return v === undefined ? null : v; },
  set: function(k, v) { this._h[String(k).toLowerCase()] = String(v); },
  append: function(k, v) { k = String(k).toLowerCase(); this._h[k] = this._h[k] !== undefined ? this._h[k] + ', ' + v : String(v); },
  has: function(k) { return String(k).toLowerCase() in this._h; },
  delete: function(k) { delete this._h[String(k).toLowerCase()]; },
  forEach: function(f, t) { for (var k in this._h) f.call(t, this._h[k], k, this); },
  entries: function() { var h = this._h; return Object.keys(h).map(function(k) { return [k, h[k]]; })[Symbol.iterator](); },
  keys: function() { return Object.keys(this._h)[Symbol.iterator](); },
  values: function() { var h = this._h; return Object.keys(h).map(function(k) { return h[k]; })[Symbol.iterator](); }
});
Headers.prototype[Symbol.iterator] = Headers.prototype.entries;
g.Headers = Headers;
function headerLines(h) {
  var s = '';
  h.forEach(function(v, k) {
    if (!/^(host|cookie|content-length|connection|user-agent|referer|accept-encoding)$/.test(k))
      s += k.replace(/(^|-)[a-z]/g, function(c) { return c.toUpperCase(); }) + ': ' + v + '\r\n';
  });
  return s;
}

function Response(body, init) {
  init = init || {};
  this._body = body == null ? '' : String(body);
  this.status = init.status === undefined ? 200 : init.status;
  this.statusText = init.statusText || '';
  this.ok = this.status >= 200 && this.status < 300;
  this.headers = new Headers(init.headers);
  this.url = init.url || '';
  this.type = 'basic';
  this.redirected = false;
  this.bodyUsed = false;
}
methods(Response.prototype, {
  text: function() { this.bodyUsed = true; return Promise.resolve(this._body); },
  json: function() { this.bodyUsed = true; var b = this._body; return new Promise(function(ok) { ok(JSON.parse(b)); }); },
  arrayBuffer: function() { return Promise.resolve(new TextEncoder().encode(this._body).buffer); },
  blob: function() { return Promise.resolve(new Blob([this._body], { type: this.headers.get('content-type') || '' })); },
  clone: function() { return new Response(this._body, this); }
});
Response.json = function(d, init) { return new Response(JSON.stringify(d), init); };
Response.error = function() { return new Response('', { status: 0 }); };
g.Response = Response;
function Request(input, init) {
  init = init || {};
  this.url = ns.resolve(String(input instanceof Request ? input.url : input));
  this.method = (init.method || (input instanceof Request ? input.method : 'GET')).toUpperCase();
  this.headers = new Headers(init.headers || (input instanceof Request ? input.headers : null));
  this.body = init.body === undefined ? null : init.body;
  this.signal = init.signal || null;
  this.credentials = init.credentials || 'same-origin';
  this.mode = init.mode || 'cors';
}
g.Request = Request;
function bodyText(b) {
  if (b == null) return null;
  if (typeof b === 'string') return b;
  if (b instanceof URLSearchParamsC) return b.toString();
  if (b instanceof FormData) return b._encode();
  if (b instanceof Blob) return b._text;
  return String(b);
}
function bodyType(b, h) {
  if (h.has('content-type')) return;
  if (b instanceof URLSearchParamsC || b instanceof FormData) h.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8');
  else if (typeof b === 'string') h.set('content-type', 'text/plain;charset=UTF-8');
}
g.fetch = function(input, init) {
  var req = new Request(input, init);
  return new Promise(function(resolve, reject) {
    if (req.signal && req.signal.aborted) return reject(req.signal.reason);
    var body = bodyText(req.body);
    bodyType(req.body, req.headers);
    var aborted = false;
    if (req.signal) req.signal.addEventListener('abort', function() { aborted = true; reject(req.signal.reason); });
    ns.load(req.url, req.method, body, headerLines(req.headers), function(status, text, type, url) {
      if (aborted) return;
      if (!status) return reject(new TypeError('NetworkError when attempting to fetch resource.'));
      resolve(new Response(text, { status: status, url: url || req.url, headers: { 'content-type': type } }));
    });
  });
};

var XMLHttpRequest = klass('XMLHttpRequest', EventTarget, function XMLHttpRequest() {
  this.readyState = 0; this.status = 0; this.statusText = ''; this.response = '';
  this.responseText = ''; this.responseXML = null; this.responseURL = '';
  this.responseType = ''; this.timeout = 0; this.withCredentials = false;
  this._h = new Headers(); this._type = '';
  this.upload = new EventTarget();
});
['UNSENT', 'OPENED', 'HEADERS_RECEIVED', 'LOADING', 'DONE'].forEach(function(k, i) { XMLHttpRequest[k] = XMLHttpRequest.prototype[k] = i; });
function xhrFire(x, type) {
  var ev = new g.ProgressEvent(type);
  dispatch(x, ev);
}
methods(XMLHttpRequest.prototype, {
  open: function(method, url, async) {
    this._method = String(method).toUpperCase();
    this._url = ns.resolve(String(url));
    if (async === false) log('XMLHttpRequest: synchronous request to ' + this._url + ' made asynchronous');
    this.readyState = 1;
    this._aborted = false;
    xhrFire(this, 'readystatechange');
  },
  setRequestHeader: function(k, v) { this._h.append(k, v); },
  overrideMimeType: function() {},
  getResponseHeader: function(k) { return String(k).toLowerCase() === 'content-type' ? this._type || null : null; },
  getAllResponseHeaders: function() { return this._type ? 'content-type: ' + this._type + '\r\n' : ''; },
  abort: function() { this._aborted = true; this.readyState = 0; xhrFire(this, 'abort'); },
  send: function(body) {
    var x = this, b = this._method === 'GET' || this._method === 'HEAD' ? null : bodyText(body);
    if (b !== null) bodyType(body, this._h);
    xhrFire(this, 'loadstart');
    ns.load(this._url, this._method, b, headerLines(this._h), function(status, text, type, url) {
      if (x._aborted) return;
      x.status = status; x.statusText = status === 200 ? 'OK' : '';
      x._type = type; x.responseURL = url || x._url;
      x.readyState = 2; xhrFire(x, 'readystatechange');
      x.readyState = 3; xhrFire(x, 'readystatechange');
      x.responseText = text;
      if (x.responseType === 'json') { try { x.response = JSON.parse(text); } catch (e) { x.response = null; } }
      else if (x.responseType === 'document') {
        var d = implementation.createHTMLDocument('');
        d.body.innerHTML = text; x.response = x.responseXML = d;
      } else if (x.responseType === 'arraybuffer') x.response = new TextEncoder().encode(text).buffer;
      else x.response = text;
      x.readyState = 4; xhrFire(x, 'readystatechange');
      xhrFire(x, status ? 'load' : 'error');
      xhrFire(x, 'loadend');
    });
  }
});

// sendBeacon: fire and forget
g.navigator.sendBeacon = function(url, data) {
  ns.load(ns.resolve(String(url)), 'POST', bodyText(data) || '', '', function() {});
  return true;
};

// ---- URL, URLSearchParams, FormData, Blob, TextEncoder ------------------------

function URLSearchParamsC(init) {
  this._l = [];
  if (init == null) return;
  if (init instanceof URLSearchParamsC) this._l = init._l.slice();
  else if (typeof init === 'object') {
    if (Array.isArray(init)) this._l = init.map(function(p) { return [String(p[0]), String(p[1])]; });
    else for (var k in init) this._l.push([k, String(init[k])]);
  } else {
    String(init).replace(/^\?/, '').split('&').forEach(function(p) {
      if (!p) return;
      var i = p.indexOf('='), k = i < 0 ? p : p.slice(0, i), v = i < 0 ? '' : p.slice(i + 1);
      this._l.push([dec(k), dec(v)]);
    }, this);
  }
}
function dec(s) { try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (e) { return s; } }
function enc(s) { return encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, function(c) { return '%' + c.charCodeAt(0).toString(16).toUpperCase(); }); }
methods(URLSearchParamsC.prototype, {
  get: function(k) { for (var i = 0; i < this._l.length; i++) if (this._l[i][0] === k) return this._l[i][1]; return null; },
  getAll: function(k) { return this._l.filter(function(p) { return p[0] === k; }).map(function(p) { return p[1]; }); },
  has: function(k) { return this.get(k) !== null; },
  set: function(k, v) {
    var i = 0, found = false;
    k = String(k);
    this._l = this._l.filter(function(p) { if (p[0] !== k) return true; if (found) return false; found = true; p[1] = String(v); return true; });
    if (!found) this._l.push([k, String(v)]);
    this._sync();
  },
  append: function(k, v) { this._l.push([String(k), String(v)]); this._sync(); },
  delete: function(k) { this._l = this._l.filter(function(p) { return p[0] !== k; }); this._sync(); },
  sort: function() { this._l.sort(function(a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); this._sync(); },
  forEach: function(f, t) { this._l.forEach(function(p) { f.call(t, p[1], p[0], this); }, this); },
  keys: function() { return this._l.map(function(p) { return p[0]; })[Symbol.iterator](); },
  values: function() { return this._l.map(function(p) { return p[1]; })[Symbol.iterator](); },
  entries: function() { return this._l.map(function(p) { return [p[0], p[1]]; })[Symbol.iterator](); },
  toString: function() { return this._l.map(function(p) { return enc(p[0]) + '=' + enc(p[1]); }).join('&'); },
  _sync: function() { if (this._url) { var s = this.toString(); this._url._search = s ? '?' + s : ''; } }
});
URLSearchParamsC.prototype[Symbol.iterator] = URLSearchParamsC.prototype.entries;
getters(URLSearchParamsC.prototype, { size: function() { return this._l.length; } });
Object.defineProperty(URLSearchParamsC, 'name', { value: 'URLSearchParams' });
g.URLSearchParams = URLSearchParamsC;

var urlRe = /^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\/\/(?:([^:@\/?#]*)(?::([^@\/?#]*))?@)?(\[[^\]]*\]|[^:\/?#]*)(?::([0-9]*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
var defaultPorts = { 'http:': '80', 'https:': '443', 'ftp:': '21' };
function URLc(url, base) {
  if (!(this instanceof URLc)) throw new TypeError("Constructor URL requires 'new'");
  url = String(url);
  var abs;
  if (base !== undefined) {
    base = String(base instanceof URLc ? base.href : base);
    if (!urlRe.test(base)) throw new TypeError('Invalid base URL: ' + base);
    abs = ns.resolve(url, base);
  } else abs = url;
  var m = urlRe.exec(abs.trim());
  if (!m) throw new TypeError('Invalid URL: ' + url);
  this._protocol = m[1].toLowerCase();
  this._user = m[2] || ''; this._pass = m[3] || '';
  this._hostname = (m[4] || '').toLowerCase();
  this._port = m[5] && m[5] !== defaultPorts[this._protocol] ? m[5] : '';
  this._path = m[6] || (m[4] !== undefined ? '/' : '');
  this._search = m[7] && m[7] !== '?' ? m[7] : '';
  this._hash = m[8] && m[8] !== '#' ? m[8] : '';
  this._special = m[4] !== undefined;
}
getters(URLc.prototype, {
  href: [function() {
    var auth = this._user ? this._user + (this._pass ? ':' + this._pass : '') + '@' : '';
    return this._protocol + (this._special ? '//' + auth + this.host : '') + this._path + this._search + this._hash;
  }, function(v) { var u = new URLc(v); for (var k in u) this[k] = u[k]; }],
  protocol: [function() { return this._protocol; }, function(v) { this._protocol = String(v).replace(/:?$/, ':'); }],
  username: [function() { return this._user; }, function(v) { this._user = String(v); }],
  password: [function() { return this._pass; }, function(v) { this._pass = String(v); }],
  host: [function() { return this._hostname + (this._port ? ':' + this._port : ''); },
         function(v) { var p = String(v).split(':'); this._hostname = p[0]; this._port = p[1] || ''; }],
  hostname: [function() { return this._hostname; }, function(v) { this._hostname = String(v); }],
  port: [function() { return this._port; }, function(v) { this._port = String(v); }],
  pathname: [function() { return this._path; }, function(v) { v = String(v); this._path = v.charAt(0) === '/' ? v : '/' + v; }],
  search: [function() { return this._search; }, function(v) { v = String(v); this._search = v && v !== '?' ? (v.charAt(0) === '?' ? v : '?' + v) : ''; if (this._sp) this._sp._l = new URLSearchParamsC(this._search)._l; }],
  hash: [function() { return this._hash; }, function(v) { v = String(v); this._hash = v && v !== '#' ? (v.charAt(0) === '#' ? v : '#' + v) : ''; }],
  origin: function() { return this._special ? this._protocol + '//' + this.host : 'null'; },
  searchParams: function() {
    if (!this._sp) { this._sp = new URLSearchParamsC(this._search); this._sp._url = this; }
    return this._sp;
  }
});
methods(URLc.prototype, { toString: function() { return this.href; }, toJSON: function() { return this.href; } });
URLc.canParse = function(u, b) { try { new URLc(u, b); return true; } catch (e) { return false; } };
URLc.parse = function(u, b) { try { return new URLc(u, b); } catch (e) { return null; } };
URLc.createObjectURL = function() { return 'blob:' + ns.url() + '#' + Math.random(); };
URLc.revokeObjectURL = function() {};
Object.defineProperty(URLc, 'name', { value: 'URL' });
g.URL = g.webkitURL = URLc;

function FormData(form) {
  this._l = [];
  if (form && form.elements) {
    var els = form.elements;
    for (var i = 0; i < els.length; i++) {
      var e = els[i], n = e.name, t = (e.type || '').toLowerCase();
      if (!n || e.disabled || /^(submit|button|reset|image|file)$/.test(t)) continue;
      if (/^(checkbox|radio)$/.test(t) && !e.checked) continue;
      if (e.tagName === 'SELECT') { e.selectedOptions.forEach(function(o) { this._l.push([n, o.value]); }, this); continue; }
      this._l.push([n, String(e.value)]);
    }
  }
}
FormData.prototype = Object.create(URLSearchParamsC.prototype);
FormData.prototype.constructor = FormData;
FormData.prototype._encode = function() { return URLSearchParamsC.prototype.toString.call(this); };
FormData.prototype._sync = function() {};
g.FormData = FormData;

function submitForm(f) {
  var action = f.action || ns.url(), method = (D.attr(f, 'method') || 'get').toLowerCase();
  var q = new FormData(f)._encode();
  if (method === 'post') log('form.submit(): POST not supported from script; submitting as GET');
  g.location.href = action.replace(/[?#].*$/, '') + (q ? '?' + q : '');
}

function Blob(parts, opts) {
  this._text = (parts || []).map(function(p) { return p instanceof Blob ? p._text : String(p); }).join('');
  this.size = this._text.length;
  this.type = (opts && opts.type) || '';
}
methods(Blob.prototype, {
  text: function() { return Promise.resolve(this._text); },
  arrayBuffer: function() { return Promise.resolve(new TextEncoder().encode(this._text).buffer); },
  slice: function(a, b, t) { return new Blob([this._text.slice(a, b)], { type: t }); }
});
g.Blob = Blob;
g.File = function File(parts, name, opts) { Blob.call(this, parts, opts); this.name = name; this.lastModified = Date.now(); };
g.File.prototype = Object.create(Blob.prototype);
g.FileReader = function FileReader() { this.readyState = 0; this.result = null; };
methods(g.FileReader.prototype, {
  readAsText: function(b) { var r = this; setTimeout(function() { r.result = b._text; r.readyState = 2; if (r.onload) r.onload({ target: r }); if (r.onloadend) r.onloadend({ target: r }); }, 0); },
  readAsDataURL: function(b) { var r = this; setTimeout(function() { r.result = 'data:' + b.type + ';base64,' + g.btoa(unescape(encodeURIComponent(b._text))); r.readyState = 2; if (r.onload) r.onload({ target: r }); if (r.onloadend) r.onloadend({ target: r }); }, 0); },
  abort: function() {}, addEventListener: function(t, f) { this['on' + t] = f; }
});

function TextEncoder() { this.encoding = 'utf-8'; }
TextEncoder.prototype.encode = function(s) {
  s = unescape(encodeURIComponent(s === undefined ? '' : String(s)));
  var a = new Uint8Array(s.length);
  for (var i = 0; i < s.length; i++) a[i] = s.charCodeAt(i);
  return a;
};
function TextDecoder(label) { this.encoding = (label || 'utf-8').toLowerCase(); }
TextDecoder.prototype.decode = function(b) {
  if (!b) return '';
  var a = b instanceof ArrayBuffer ? new Uint8Array(b) : new Uint8Array(b.buffer || b, b.byteOffset || 0, b.byteLength), s = '';
  for (var i = 0; i < a.length; i += 8192) s += String.fromCharCode.apply(null, a.subarray(i, i + 8192));
  if (this.encoding !== 'utf-8' && this.encoding !== 'utf8') return s;
  try { return decodeURIComponent(escape(s)); } catch (e) { return s; }
};
if (!g.TextEncoder) g.TextEncoder = TextEncoder;
if (!g.TextDecoder) g.TextDecoder = TextDecoder;

// ---- window things that need the DOM ------------------------------------------

var displayOf = /^(DIV|P|H[1-6]|UL|OL|LI|DL|DT|DD|TABLE|FORM|BLOCKQUOTE|PRE|HR|ADDRESS|CENTER|SECTION|ARTICLE|ASIDE|HEADER|FOOTER|NAV|MAIN|FIGURE|FIGCAPTION|FIELDSET|DETAILS|SUMMARY|HTML|BODY)$/;
g.getComputedStyle = function(el) {
  var own = el && el.nodeType === 1 ? parseStyle(D.attr(el, 'style')) : new Map();
  function value(p) {
    p = cssName(String(p));
    if (own.has(p)) return own.get(p);
    if (p === 'display') {
      if (el.nodeType === 1 && D.hasAttr(el, 'hidden')) return 'none';
      var t = el.tagName || '';
      if (/^(SCRIPT|STYLE|HEAD|META|LINK|TITLE|TEMPLATE|NOSCRIPT)$/.test(t)) return 'none';
      if (t === 'TR') return 'table-row';
      if (t === 'TD' || t === 'TH') return 'table-cell';
      return displayOf.test(t) ? 'block' : 'inline';
    }
    if (p === 'visibility') return 'visible';
    if (p === 'opacity') return '1';
    if (p === 'position') return 'static';
    if (p === 'direction') return 'ltr';
    if (p === 'font-size') return '16px';
    if (p === 'line-height') return 'normal';
    if (p === 'color') return 'rgb(0, 0, 0)';
    if (p === 'background-color') return 'rgba(0, 0, 0, 0)';
    if (/^(width|height)$/.test(p)) return 'auto';
    if (/^(margin|padding|border).*(width|top|left|right|bottom)$/.test(p)) return '0px';
    return '';
  }
  return new Proxy({}, {
    get: function(t, p) {
      if (typeof p === 'symbol') return undefined;
      if (p === 'getPropertyValue') return value;
      if (p === 'getPropertyPriority') return function() { return ''; };
      if (p === 'length') return 0;
      if (p === 'cssText') return '';
      return value(p);
    }
  });
};
g.matchMedia = function(q) {
  q = String(q);
  var w = g.innerWidth, h = g.innerHeight, m = true;
  q.replace(/\(\s*(min|max)-(width|height)\s*:\s*([\d.]+)(px|em|rem)?\s*\)/g, function(s, mm, dim, n, unit) {
    var v = parseFloat(n) * (unit === 'em' || unit === 'rem' ? 16 : 1), x = dim === 'width' ? w : h;
    if (mm === 'min' ? x < v : x > v) m = false;
  });
  if (/prefers-color-scheme:\s*dark|prefers-reduced-motion:\s*reduce|\bprint\b|hover:\s*none|pointer:\s*coarse|orientation:\s*portrait/.test(q)) m = false;
  if (/^\s*not\s/.test(q)) m = !m;
  var mql = new EventTarget();
  mql.matches = m; mql.media = q; mql.onchange = null;
  mql.addListener = function() {}; mql.removeListener = function() {};
  return mql;
};
var ceDefs = Object.create(null), ceNames = new Map(), ceWaiting = Object.create(null);
var CE = Symbol('custom');			// the element's definition, once upgraded
function ceUpgrade(el) {
  if (el[CE]) return;
  var def = ceDefs[D.name(el).toLowerCase()];
  if (!def) return;
  Object.defineProperty(el, CE, { value: def, configurable: true });
  upgrading = el;
  try { Reflect.construct(def.C, []); }
  catch (x) { report('custom element ' + def.name, x); }
  upgrading = null;
  if (def.C.prototype.attributeChangedCallback) {
    def.observed.forEach(function(a) {
      var v = D.attr(el, a);
      if (v !== null) ceCall(el, 'attributeChangedCallback', [a, null, v]);
    });
  }
  if (isConnected(el)) ceCall(el, 'connectedCallback', []);
}
function ceCall(el, cb, args) {
  if (typeof el[cb] !== 'function') return;
  try { el[cb].apply(el, args); } catch (x) { report(cb, x); }
}
// Upgrade the defined custom elements in ROOT (and ROOT itself).
function ceUpgradeIn(root) {
  var names = Object.keys(ceDefs);
  if (!names.length || !root) return;
  if (root.nodeType === 1) ceUpgrade(root);
  if (root.nodeType === 1 || root.nodeType === 9 || root.nodeType === 11)
    D.select(root, names.join(','), false).forEach(ceUpgrade);
}
function ceConnected(nodes, connected) {
  if (!Object.keys(ceDefs).length) return;
  nodes.forEach(function(n) {
    if (!n || n.nodeType !== 1) return;
    var all = [n].concat(D.select(n, Object.keys(ceDefs).join(','), false));
    all.forEach(function(el) {
      if (!el[CE]) { if (connected) ceUpgrade(el); return; }
      ceCall(el, connected ? 'connectedCallback' : 'disconnectedCallback', []);
    });
  });
}
g.customElements = {
  define: function(name, C, opts) {
    name = String(name).toLowerCase();
    if (ceDefs[name]) throw new TypeError("NotSupportedError: '" + name + "' has already been defined");
    // read when defined, as browsers do (libraries finalize their classes
    // in this getter: Lit installs its reactive properties there)
    var observed = C.observedAttributes;
    ceDefs[name] = { name: name, C: C, observed: observed ? [].slice.call(observed) : [] };
    ceNames.set(C, name);
    protos[name.toUpperCase()] = C.prototype;
    D.protos(protos);
    ceUpgradeIn(document);
    (ceWaiting[name] || []).forEach(function(f) { f(C); });
    delete ceWaiting[name];
  },
  get: function(name) { var d = ceDefs[String(name).toLowerCase()]; return d ? d.C : undefined; },
  getName: function(C) { return ceNames.get(C) || null; },
  whenDefined: function(name) {
    name = String(name).toLowerCase();
    if (ceDefs[name]) return Promise.resolve(ceDefs[name].C);
    return new Promise(function(ok) { (ceWaiting[name] = ceWaiting[name] || []).push(ok); });
  },
  upgrade: function(root) { ceUpgradeIn(root); }
};
g.requestIdleCallback = function(f) { return g.setTimeout(function() { f({ didTimeout: false, timeRemaining: function() { return 50; } }); }, 1); };
g.cancelIdleCallback = function(id) { g.clearTimeout(id); };
g.structuredClone = function(v) { return v === undefined ? v : JSON.parse(JSON.stringify(v)); };
g.reportError = function(e) { report('reportError', e); };
var perfStart = Date.now();
if (!g.performance) g.performance = {
  now: function() { return Date.now() - perfStart; }, timeOrigin: perfStart,
  timing: { navigationStart: perfStart }, navigation: { type: 0 },
  mark: function() {}, measure: function() {}, clearMarks: function() {}, clearMeasures: function() {},
  getEntries: function() { return []; }, getEntriesByType: function() { return []; },
  getEntriesByName: function() { return []; }
};
if (!g.crypto) g.crypto = {
  getRandomValues: function(a) { for (var i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 4294967296); return a; },
  randomUUID: function() {
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function(c) {
      var r = Math.random() * 16 | 0; return (c === 'x' ? r : (r & 3 | 8)).toString(16); });
  }
};
g.Image = function Image(w, h) {
  var i = document.createElement('img');
  if (w !== undefined) i.setAttribute('width', String(w));
  if (h !== undefined) i.setAttribute('height', String(h));
  return i;
};
g.Image.prototype = g.HTMLImageElement.prototype;
g.Node = Node; g.Element = Element; g.HTMLElement = HTMLElement; g.Document = Document;
g.HTMLCollection = g.NodeList = Array;
// constructable style sheets (Lit and other web component libraries)
function CSSStyleSheet() { this.cssRules = []; this.rules = this.cssRules; this.disabled = false; this._text = ''; }
methods(CSSStyleSheet.prototype, {
  replaceSync: function(t) { this._text = String(t); },
  replace: function(t) { this._text = String(t); return Promise.resolve(this); },
  insertRule: function(r, i) { this.cssRules.splice(i || 0, 0, { cssText: String(r) }); return i || 0; },
  deleteRule: function(i) { this.cssRules.splice(i, 1); },
  addRule: function() { return -1; }, removeRule: function() {}
});
g.CSSStyleSheet = CSSStyleSheet;
g.ShadowRoot = g.ShadowRoot || function ShadowRoot() { throw new TypeError('Illegal constructor'); };
Object.defineProperty(Document.prototype, 'adoptedStyleSheets', {
  get: function() { return this._adopted || (this._adopted = []); },
  set: function(v) { this._adopted = v; }, configurable: true });
g.CSS = { supports: function() { return false; }, escape: function(s) { return String(s).replace(/[^a-zA-Z0-9_-]/g, '\\$&'); } };
g.DOMParser = function DOMParser() {};
g.DOMParser.prototype.parseFromString = function(s) {
  var d = implementation.createHTMLDocument('');
  d.body.innerHTML = String(s);
  return d;
};
g.XMLSerializer = function XMLSerializer() {};
g.XMLSerializer.prototype.serializeToString = function(n) { return D.html(n, true); };
g.MessageChannel = function MessageChannel() {
  var a = new EventTarget(), b = new EventTarget();
  a.postMessage = function(d) { setTimeout(function() { var e = new g.MessageEvent('message', { data: d }); if (b.onmessage) b.onmessage(e); dispatch(b, e); }, 0); };
  b.postMessage = function(d) { setTimeout(function() { var e = new g.MessageEvent('message', { data: d }); if (a.onmessage) a.onmessage(e); dispatch(a, e); }, 0); };
  a.start = b.start = a.close = b.close = function() {};
  this.port1 = a; this.port2 = b;
};
g.postMessage = function(d) {
  setTimeout(function() { dispatch(g, new g.MessageEvent('message', { data: d, origin: g.location.origin, source: g })); }, 0);
};
g.BroadcastChannel = function BroadcastChannel(n) { this.name = n; };
methods(g.BroadcastChannel.prototype, { postMessage: function() {}, close: function() {}, addEventListener: function() {}, removeEventListener: function() {} });
g.Worker = function Worker() { throw new Error('Workers are not supported'); };
g.history.scrollRestoration = 'auto';

// ---- the document object, and the hooks C calls -------------------------------

var protos = { '0': Node.prototype, '1': HTMLElement.prototype, '3': Text.prototype,
  '4': Text.prototype, '8': Comment.prototype, '9': HTMLDocument.prototype,
  '11': DocumentFragment.prototype };
for (var t in tagClasses) protos[t] = tagClasses[t];
D.protos(protos);
document = D.document();
g.document = document;

// what a click on an element does when no handler stops it, for clicks made
// by script (el.click()): layout does it for real clicks
function defaultAction(el, type) {
  if (type !== 'click') return;
  var a = el.closest ? el.closest('a[href]') : null;
  if (a) {
    var href = a.href;
    if (/^javascript:/i.test(href)) {
      try { (0, eval)(decodeURIComponent(href.slice(11))); } catch (x) { report('javascript: URL', x); }
    } else g.location.href = href;
    return;
  }
  var t = (el.type || '').toLowerCase();
  if (el.tagName === 'INPUT' && /^(checkbox|radio)$/.test(t)) el.checked = !el.checked;
  if ((el.tagName === 'BUTTON' && t === 'submit') || (el.tagName === 'INPUT' && t === 'submit')) {
    var f = el.form;
    if (f && dispatch(f, new Event('submit', { bubbles: true, cancelable: true }))) submitForm(f);
  }
}

// An event from layout on node N: false if a handler cancelled it.
g.__ns_event = function(n, type, init) {
  var ev;
  if (/^(click|dblclick|mouse(down|up|over|out|move))$/.test(type)) {
    init.bubbles = true; init.cancelable = type !== 'mouseout';
    init.button = 0; init.buttons = /down/.test(type) ? 1 : 0;
    init.ctrlKey = !!(init.modifiers & 2); init.shiftKey = !!(init.modifiers & 4);
    init.altKey = !!(init.modifiers & 1); init.metaKey = !!(init.modifiers & 8);
    init.detail = type === 'dblclick' ? 2 : 1;
    init.view = g;
    ev = new MouseEvent(type, init);
  } else if (/^key/.test(type)) {
    init.bubbles = init.cancelable = true;
    init.keyCode = init.which; init.charCode = type === 'keypress' ? init.which : 0;
    init.key = init.which ? String.fromCharCode(init.which) : '';
    if (init.which === 13) init.key = 'Enter';
    ev = new g.KeyboardEvent(type, init);
  } else if (type === 'focus' || type === 'blur') {
    ev = new g.FocusEvent(type, {});
  } else if (type === 'submit' || type === 'reset') {
    if (n.tagName !== 'FORM') n = n.form || n.closest('form') || n;
    // a control in no form (layout gave it one of its own) submits nothing
    if (n.tagName !== 'FORM') return false;
    ev = new Event(type, { bubbles: true, cancelable: true });
  } else {
    ev = new Event(type, { bubbles: true, cancelable: false });
    if (type === 'change') dispatch(n, new g.InputEvent('input', { bubbles: true }));
  }
  ev.isTrusted = true;
  return dispatch(n, ev);
};

// document.NAME for named forms and images, form.NAME for form controls
// (by name or id): C gives the ones layout made before each script runs.
function namedProp(obj, key, get) {
  if (!key || key in obj || /^\d+$/.test(key)) return;
  Object.defineProperty(obj, key, { get: get, configurable: true, enumerable: false });
}
g.__ns_names = function(els) {
  ceUpgradeIn(document);
  els.forEach(function(el) {
    var tag = D.name(el), name = D.attr(el, 'name'), id = D.attr(el, 'id');
    if (/^(FORM|IMG|EMBED|OBJECT|APPLET|IFRAME)$/.test(tag) && name)
      namedProp(document, name, function() {
        var a = D.select(document, '[name="' + name.replace(/["\\]/g, '\\$&') + '"]', false)
          .filter(function(e) { return /^(FORM|IMG|EMBED|OBJECT|APPLET|IFRAME)$/.test(D.name(e)); });
        return a.length > 1 ? list(a) : a[0];
      });
    if (/^(FORM|IMG|EMBED|OBJECT|APPLET|IFRAME)$/.test(tag)) return;
    var f = el.form;
    if (!f) return;
    [name, id].forEach(function(key) {
      if (!key) return;
      namedProp(f, key, function() {
        var q = key.replace(/["\\]/g, '\\$&');
        var a = D.select(f, '[name="' + q + '"],[id="' + q + '"]', false);
        return a.length > 1 ? list(a) : a[0];
      });
    });
  });
};

// the document's load events (qjs_window.c qjs_fire)
g.__ns_fire = function(type) {
  if (type === 'load') {
    runPageModules(fireLoad);
    return;
  }
  if (type === 'unload') {
    dispatch(g, new g.PageTransitionEvent('pagehide'));
    dispatch(g, new Event('unload'));
  } else {
    dispatch(g, new Event(type));
  }
};
function fireLoad() {
  ceUpgradeIn(document);
  {
    readyState = 'interactive';
    dispatch(document, new Event('readystatechange'));
    dispatch(document, new Event('DOMContentLoaded', { bubbles: true }));
    readyState = 'complete';
    dispatch(document, new Event('readystatechange'));
    dispatch(g, new Event('load'));
    dispatch(g, new g.PageTransitionEvent('pageshow'));
  }
}
})(globalThis, globalThis.__ns);
