/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const EXPORTED_SYMBOLS = ["MarionetteFrameChild"];

const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);

XPCOMUtils.defineLazyModuleGetters(this, {
  atom: "chrome://marionette/content/atom.js",
  ContentDOMReference: "resource://gre/modules/ContentDOMReference.jsm",
  element: "chrome://marionette/content/element.js",
  error: "chrome://marionette/content/error.js",
  evaluate: "chrome://marionette/content/evaluate.js",
  interaction: "chrome://marionette/content/interaction.js",
  Log: "chrome://marionette/content/log.js",
  pprint: "chrome://marionette/content/format.js",
  WebElement: "chrome://marionette/content/element.js",
});

XPCOMUtils.defineLazyGetter(this, "logger", () => Log.get());

class MarionetteFrameChild extends JSWindowActorChild {
  constructor() {
    super();
  }

  get innerWindowId() {
    return this.manager.innerWindowId;
  }

  actorCreated() {
    logger.trace(
      `[${this.browsingContext.id}] Child actor created for window id ${this.innerWindowId}`
    );
  }

  handleEvent(event) {
    switch (event.type) {
      case "beforeunload":
      case "pagehide":
      case "DOMContentLoaded":
      case "pageshow":
        this.sendAsyncMessage("MarionetteFrameChild:PageLoadEvent", {
          browsingContext: this.browsingContext,
          innerWindowId: this.innerWindowId,
          type: event.type,
        });
        break;
    }
  }

  async receiveMessage(msg) {
    const { name, data: serializedData } = msg;
    const data = evaluate.fromJSON(serializedData);

    try {
      let result;

      switch (name) {
        case "MarionetteFrameParent:clearElement":
          this.clearElement(data);
          break;
        case "MarionetteFrameParent:clickElement":
          result = await this.clickElement(data);
          break;
        case "MarionetteFrameParent:findElement":
          result = await this.findElement(data);
          break;
        case "MarionetteFrameParent:findElements":
          result = await this.findElements(data);
          break;
        case "MarionetteFrameParent:getCurrentUrl":
          result = await this.getCurrentUrl();
          break;
        case "MarionetteFrameParent:getActiveElement":
          result = await this.getActiveElement();
          break;
        case "MarionetteFrameParent:getElementAttribute":
          result = await this.getElementAttribute(data);
          break;
        case "MarionetteFrameParent:getElementProperty":
          result = await this.getElementProperty(data);
          break;
        case "MarionetteFrameParent:getElementRect":
          result = await this.getElementRect(data);
          break;
        case "MarionetteFrameParent:getElementTagName":
          result = await this.getElementTagName(data);
          break;
        case "MarionetteFrameParent:getElementText":
          result = await this.getElementText(data);
          break;
        case "MarionetteFrameParent:getElementValueOfCssProperty":
          result = await this.getElementValueOfCssProperty(data);
          break;
        case "MarionetteFrameParent:getPageSource":
          result = await this.getPageSource();
          break;
        case "MarionetteFrameParent:isElementDisplayed":
          result = await this.isElementDisplayed(data);
          break;
        case "MarionetteFrameParent:isElementEnabled":
          result = await this.isElementEnabled(data);
          break;
        case "MarionetteFrameParent:isElementSelected":
          result = await this.isElementSelected(data);
          break;
        case "MarionetteFrameParent:sendKeysToElement":
          result = await this.sendKeysToElement(data);
          break;
        case "MarionetteFrameParent:switchToFrame":
          result = await this.switchToFrame(data);
          break;
        case "MarionetteFrameParent:switchToParentFrame":
          result = await this.switchToParentFrame();
          break;
      }

      return { data: evaluate.toJSON(result) };
    } catch (e) {
      // Always wrap errors as WebDriverError
      return { error: error.wrap(e).toJSON() };
    }
  }

  /**
   * Wrapper around ContentDOMReference.get with additional steps specific to
   * Marionette.
   *
   * @param {Element} el
   *     The DOM element to generate the identifier for.
   *
   * @return {object} The ContentDOMReference ElementIdentifier for the DOM
   *     element augmented with a Marionette WebElement reference.
   */
  async getElementId(el) {
    const id = ContentDOMReference.get(el);
    const webEl = WebElement.from(el);
    id.webElRef = webEl.toJSON();
    // Use known WebElement reference if parent process has seen `id` before
    // TODO - Bug 1666837 - Avoid interprocess element lookup when possible
    id.webElRef = await this.sendQuery(
      "MarionetteFrameChild:ElementIdCacheAdd",
      id
    );
    return id;
  }

  /**
   * Wrapper around ContentDOMReference.resolve with additional error handling
   * specific to Marionette.
   *
   * @param {ElementIdentifier} id
   *     The identifier generated via ContentDOMReference.get for a DOM element.
   *
   * @return {Element} The DOM element that the identifier was generated for, or
   *     null if the element does not still exist.
   *
   * @throws {StaleElementReferenceError}
   *     If the element has gone stale, indicating it is no longer
   *     attached to the DOM, or its node document is no longer the
   *     active document.
   */
  resolveElement(id) {
    let webEl;
    if (id.webElRef) {
      webEl = WebElement.fromJSON(id.webElRef);
    }
    const el = ContentDOMReference.resolve(id);
    if (element.isStale(el, this.contentWindow)) {
      throw new error.StaleElementReferenceError(
        pprint`The element reference of ${el || webEl?.uuid} is stale; ` +
          "either the element is no longer attached to the DOM, " +
          "it is not in the current frame context, " +
          "or the document has been refreshed"
      );
    }
    return el;
  }

  // Implementation of WebDriver commands

  /** Clear the text of an element.
   *
   * @param {Object} options
   * @param {ElementIdentifier} options.webEl
   */
  clearElement(options = {}) {
    const { webEl } = options;
    const el = this.resolveElement(webEl);
    interaction.clearElement(el);
  }

  /**
   * Click an element.
   */
  async clickElement(options = {}) {
    const { capabilities, webEl } = options;
    const el = this.resolveElement(webEl);

    return interaction.clickElement(
      el,
      capabilities["moz:accessibilityChecks"],
      capabilities["moz:webdriverClick"]
    );
  }

  /**
   * Find an element in the current browsing context's document using the
   * given search strategy.
   *
   * @param {Object} options
   * @param {Object} options.opts
   * @param {ElementIdentifier} opts.startNode
   * @param {string} opts.strategy
   * @param {string} opts.selector
   *
   */
  async findElement(options = {}) {
    const { strategy, selector, opts } = options;

    opts.all = false;

    if (opts.startNode) {
      opts.startNode = this.resolveElement(opts.startNode);
    }

    const container = { frame: this.contentWindow };
    const el = await element.find(container, strategy, selector, opts);
    return this.getElementId(el);
  }

  /**
   * Find elements in the current browsing context's document using the
   * given search strategy.
   *
   * @param {Object} options
   * @param {Object} options.opts
   * @param {ElementIdentifier} opts.startNode
   * @param {string} opts.strategy
   * @param {string} opts.selector
   *
   */
  async findElements(options = {}) {
    const { strategy, selector, opts } = options;

    opts.all = true;

    if (opts.startNode) {
      opts.startNode = this.resolveElement(opts.startNode);
    }

    const container = { frame: this.contentWindow };
    const els = await element.find(container, strategy, selector, opts);
    return Promise.all(els.map(el => this.getElementId(el)));
  }

  /**
   * Return the active element in the document.
   */
  async getActiveElement() {
    let el = this.document.activeElement;
    if (!el) {
      throw new error.NoSuchElementError();
    }

    return this.getElementId(el);
  }

  /**
   * Get the current URL.
   */
  async getCurrentUrl() {
    return this.contentWindow.location.href;
  }

  /**
   * Get the value of an attribute for the given element.
   */
  async getElementAttribute(options = {}) {
    const { name, webEl } = options;
    const el = this.resolveElement(webEl);

    if (element.isBooleanAttribute(el, name)) {
      if (el.hasAttribute(name)) {
        return "true";
      }
      return null;
    }
    return el.getAttribute(name);
  }

  /**
   * Get the value of a property for the given element.
   */
  async getElementProperty(options = {}) {
    const { name, webEl } = options;
    const el = this.resolveElement(webEl);

    return typeof el[name] != "undefined" ? el[name] : null;
  }

  /**
   * Get the position and dimensions of the element.
   */
  async getElementRect(options = {}) {
    const { webEl } = options;
    const el = this.resolveElement(webEl);

    const rect = el.getBoundingClientRect();
    return {
      x: rect.x + this.content.pageXOffset,
      y: rect.y + this.content.pageYOffset,
      width: rect.width,
      height: rect.height,
    };
  }

  /**
   * Get the tagName for the given element.
   */
  async getElementTagName(options = {}) {
    const { webEl } = options;
    const el = this.resolveElement(webEl);
    return el.tagName.toLowerCase();
  }

  /**
   * Get the text content for the given element.
   */
  async getElementText(options = {}) {
    const { webEl } = options;
    const el = this.resolveElement(webEl);
    return atom.getElementText(el, this.contentWindow);
  }

  /**
   * Get the value of a css property for the given element.
   */
  async getElementValueOfCssProperty(options = {}) {
    const { name, webEl } = options;
    const el = this.resolveElement(webEl);

    const style = this.contentWindow.getComputedStyle(el);
    return style.getPropertyValue(name);
  }

  /**
   * Get the source of the current browsing context's document.
   */
  async getPageSource() {
    return this.document.documentElement.outerHTML;
  }

  /**
   * Determine the element displayedness of the given web element.
   */
  async isElementDisplayed(options = {}) {
    const { capabilities, webEl } = options;
    const el = this.resolveElement(webEl);

    return interaction.isElementDisplayed(
      el,
      capabilities["moz:accessibilityChecks"]
    );
  }

  /**
   * Check if element is enabled.
   */
  async isElementEnabled(options = {}) {
    const { capabilities, webEl } = options;
    const el = this.resolveElement(webEl);

    return interaction.isElementEnabled(
      el,
      capabilities["moz:accessibilityChecks"]
    );
  }

  /**
   * Determine whether the referenced element is selected or not.
   */
  async isElementSelected(options = {}) {
    const { capabilities, webEl } = options;
    const el = this.resolveElement(webEl);

    return interaction.isElementSelected(
      el,
      capabilities["moz:accessibilityChecks"]
    );
  }

  /*
   * Send key presses to element after focusing on it.
   */
  async sendKeysToElement(options = {}) {
    const { capabilities, text, webEl } = options;
    const el = this.resolveElement(webEl);

    const opts = {
      strictFileInteractability: capabilities.strictFileInteractability,
      accessibilityChecks: capabilities["moz:accessibilityChecks"],
      webdriverClick: capabilities["moz:webdriverClick"],
    };

    return interaction.sendKeysToElement(el, text, opts);
  }

  /**
   * Switch to the specified frame.
   *
   * @param {Object=} options
   * @param {(number|ElementIdentifier)=} options.id
   *     Identifier of the frame to switch to. If it's a number treat it as
   *     the index for all the existing frames. If it's an ElementIdentifier switch
   *     to this specific frame. If not specified or `null` switch to the
   *     top-level browsing context.
   */
  async switchToFrame(options = {}) {
    const { id } = options;

    const childContexts = this.browsingContext.children;
    let browsingContext;

    if (id == null) {
      browsingContext = this.browsingContext.top;
    } else if (typeof id == "number") {
      if (id < 0 || id >= childContexts.length) {
        throw new error.NoSuchFrameError(
          `Unable to locate frame with index: ${id}`
        );
      }
      browsingContext = childContexts[id];
      await this.getElementId(browsingContext.embedderElement);
    } else {
      const frameElement = this.resolveElement(id);
      const context = childContexts.find(context => {
        return context.embedderElement === frameElement;
      });
      if (!context) {
        throw new error.NoSuchFrameError(
          `Unable to locate frame for element: ${id}`
        );
      }
      browsingContext = context;
    }

    return { browsingContextId: browsingContext.id };
  }

  /**
   * Switch to the parent frame.
   */
  async switchToParentFrame() {
    const browsingContext = this.browsingContext.parent || this.browsingContext;

    return { browsingContextId: browsingContext.id };
  }
}
