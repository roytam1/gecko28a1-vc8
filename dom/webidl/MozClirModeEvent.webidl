/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

[Pref="dom.mobileconnection.enabled",
 Constructor(DOMString type, optional MozClirModeEventInit eventInitDict)]
interface MozClirModeEvent : Event
{
  /**
   * Indicates the mode of calling line id restriction (CLIR).
   *
   * @see nsIDOMMozMobileConnection.CLIR_* values.
   */
  readonly attribute unsigned long mode;
};

dictionary MozClirModeEventInit : EventInit
{
  unsigned long mode = 0;
};
