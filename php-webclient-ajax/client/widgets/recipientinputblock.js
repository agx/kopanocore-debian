/*
 * Copyright 2005 - 2014  Zarafa B.V.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3, 
 * as published by the Free Software Foundation with the following additional 
 * term according to sec. 7:
 *  
 * According to sec. 7 of the GNU Affero General Public License, version
 * 3, the terms of the AGPL are supplemented with the following terms:
 * 
 * "Zarafa" is a registered trademark of Zarafa B.V. The licensing of
 * the Program under the AGPL does not imply a trademark license.
 * Therefore any rights, title and interest in our trademarks remain
 * entirely with us.
 * 
 * However, if you propagate an unmodified version of the Program you are
 * allowed to use the term "Zarafa" to indicate that you distribute the
 * Program. Furthermore you may use our trademarks where it is necessary
 * to indicate the intended purpose of a product or service provided you
 * use it in accordance with honest practices in industrial or commercial
 * matters.  If you want to propagate modified versions of the Program
 * under the name "Zarafa" or "Zarafa Server", you may only do so if you
 * have a written permission by Zarafa B.V. (to acquire a permission
 * please contact Zarafa at trademark@zarafa.com).
 * 
 * The interactive user interface of the software displays an attribution
 * notice containing the term "Zarafa" and/or the logo of Zarafa.
 * Interactive user interfaces of unmodified and modified versions must
 * display Appropriate Legal Notices according to sec. 5 of the GNU
 * Affero General Public License, version 3, when you propagate
 * unmodified or modified versions of the Program. In accordance with
 * sec. 7 b) of the GNU Affero General Public License, version 3, these
 * Appropriate Legal Notices must retain the logo of Zarafa or display
 * the words "Initial Development by Zarafa" if the display of the logo
 * is not reasonably feasible for technical reasons. The use of the logo
 * of Zarafa in Legal Notices is allowed for unmodified and modified
 * versions of the software.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *  
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

RecipientInputBlockWidget.prototype = new Widget;
RecipientInputBlockWidget.prototype.constructor = RecipientInputBlockWidget;
RecipientInputBlockWidget.superclass = Widget.prototype;

function RecipientInputBlockWidget(parentWidget, parentElement, windowObj){
	this.parentWidget = parentWidget;
	this.parentElement = parentElement;
	this.windowObj = windowObj;

	this.internalID = false;
	this.displayname = "";
	this.status = 0;
	this.selected = false;
	this.editMode = false;

	this.block = false;
}

RecipientInputBlockWidget.prototype.init = function(internalID, displayname, status){
	RecipientInputWidget.superclass.init(this);

	this.internalID = internalID;
	this.displayname = displayname;
	this.status = status;
	this.drawBlock();
}

RecipientInputBlockWidget.prototype.update = function(displayname, status){
	this.displayname = displayname;
	this.status = status;
	this.drawBlock();
}
RecipientInputBlockWidget.prototype.select = function(){
	this.selected = true;
	this.drawBlock();
}
RecipientInputBlockWidget.prototype.deselect = function(){
	this.selected = false;
	this.drawBlock();
}

RecipientInputBlockWidget.prototype.enableEditMode = function(){
	this.editMode = true;
	this.drawBlock();

	if(!webclient.inputmanager.hasFocus(this)){
		webclient.inputmanager.changeFocus(this.block);
	}
	this.sendEvent("editmode_enabled", this, this.internalID);
}
RecipientInputBlockWidget.prototype.disableEditMode = function(){
	this.displayname = this.getEditModeInput();
	this.editMode = false;
	this.sendEvent("displayname_changed", this, this.internalID, this.displayname);
	this.drawBlock();
}

RecipientInputBlockWidget.prototype.destructor = function(){
	dhtml.deleteElement(this.block);
	delete this.block;
}

/**
 * HTML structure of a recipient block
 * +-htmlBlockContainer--------------------------+
 * | +-htmlBlock-------------------------------+ |
 * | | +-displayname-----+ +-arrow-+ +-cross-+ | |
 * | | |                 | |       | |       | | |
 * | | +-----------------+ +-------+ +-------+ | |
 * | +-----------------------------------------+ |
 * +---------------------------------------------+
 */
RecipientInputBlockWidget.prototype.drawBlock = function(){
	// Create the main block of a recipient
	var htmlBlockContainer = dhtml.addElement(null, "span", "recipientblock_container", null, null, this.windowObj);

	var htmlBlock = dhtml.addElement(htmlBlockContainer, "span", "recipientblock", null, null, this.windowObj);
	dhtml.addEvent(this, htmlBlock, "contextmenu", this._eventRecipientBlockRightMouseClick, this.windowObj);

	this.htmlEditInputField = false;
	// Adding displayname pane
	if(!this.editMode){
		var displaynameDIV =  dhtml.addElement(htmlBlock, "span", "recipientblock_displayname", null, this.displayname, this.windowObj);
		webclient.inputmanager.removeObject(this, this.block);
		webclient.inputmanager.unbindEvent(this, "blur", this._eventInputFieldBlur);
	}else{
		this.htmlEditInputField =  dhtml.addElement(htmlBlock, "input", "recipientblock_displayname", null, null, this.windowObj);
		this.htmlEditInputField.value = this.displayname;
		webclient.inputmanager.addObject(this, htmlBlockContainer);
		
	}

	// Applying status 
	switch(this.status){
		case this.parentWidget.STATUS_PENDING:
			dhtml.addClassName(htmlBlock, "status_pending");
			break;
		case this.parentWidget.STATUS_RESOLVED:
			dhtml.addClassName(htmlBlock, "status_resolved");
			break;
		case this.parentWidget.STATUS_MULTI_RESOLVE_OPTIONS:
			dhtml.addClassName(htmlBlock, "status_multiresolveoptions");

			var multioptionsIcon =  dhtml.addElement(htmlBlock, "span", "icon icon_recipientblock_multipleoptions", null, NBSP, this.windowObj);
			dhtml.addEvent(this, multioptionsIcon, "click", this._eventShowMultipleOptionsClick, this.windowObj);

			break;
		case this.parentWidget.STATUS_INVALID:
			dhtml.addClassName(htmlBlock, "status_invalid");

			var multioptionsIcon =  dhtml.addElement(htmlBlock, "span", "icon icon_recipientblock_edit", null, NBSP, this.windowObj);
			dhtml.addEvent(this, multioptionsIcon, "click", this._eventEditInvalidRecipientClick, this.windowObj);

			break;
	}
	if(this.selected){
		dhtml.addClassName(htmlBlockContainer, "selected");
	}

	var closeIcon =  dhtml.addElement(htmlBlock, "span", "icon icon_recipientblock_close", null, NBSP, this.windowObj);
	dhtml.addEvent(this, closeIcon, "click", this._eventRemoveRecipientClick, this.windowObj);

	if(this.block){
		this.parentElement.insertBefore(htmlBlockContainer, this.block);
		dhtml.deleteElement(this.block);
	}else{
		this.parentElement.appendChild(htmlBlockContainer);
	}
	this.block = htmlBlockContainer;

	if(this.htmlEditInputField){
		this.htmlEditInputField.focus();
		dhtml.addEvent(this, this.htmlEditInputField, "keypress", this._eventHandleKeys, this.windowObj);
		webclient.inputmanager.bindEvent(this, "blur", this._eventInputFieldBlur);
	}
}

RecipientInputBlockWidget.prototype.getEditModeInput = function(){
	if(this.editMode && this.htmlEditInputField){
		return this.htmlEditInputField.value;
	}else{
		return "";
	}
}


RecipientInputBlockWidget.prototype.buildContextMenu = function(menuItems, event){
	console.log("Contextmenu:");
	if(menuItems.length > 0){
		webclient.menu.buildContextMenu(this, "", menuItems, event.clientX, event.clientY);
	}
}

// HTML Events
RecipientInputBlockWidget.prototype._eventRemoveRecipientClick = function(widget, element, event){
//	console.log("EVENTS: CLICK ON CLOSE ICON IN WIDGET:"+widget.widgetID);
	this.sendEvent("remove", this, this.internalID);
}
RecipientInputBlockWidget.prototype._eventShowMultipleOptionsClick = function(widget, element, event){
//	console.log("WIDGET BLOCK: Multiple Options: "+this.internalID);
	this.sendEvent("more_options", this, this.internalID);
}
RecipientInputBlockWidget.prototype._eventRecipientBlockRightMouseClick = function(widget, element, event){
//	console.log("WIDGET BLOCK: Context menu: "+this.internalID);
	var menuItems = new Array();
	this.sendEvent("contextmenu", this, this.internalID, menuItems);
	this.buildContextMenu(menuItems, event);
}
RecipientInputBlockWidget.prototype._eventEditInvalidRecipientClick = function(widget, element, event){
//	console.log("WIDGET BLOCK: Edit Invalid Recipient Click: "+this.internalID);
	widget.enableEditMode();
}
RecipientInputBlockWidget.prototype._eventHandleKeys = function(widget, element, event){
	switch(event.keyCode){
		case this.parentWidget.KEY_TAB:
		case this.parentWidget.KEY_ENTER:
			this.disableEditMode();
		break;
	}
}
RecipientInputBlockWidget.prototype._eventInputFieldBlur = function(widget, element, event){
//	console.log("blur edit mode");
	widget.disableEditMode();
}
