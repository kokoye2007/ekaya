/*
 * Copyright (C) 2009 ThanLwinSoft.org
 *
 * The Ekaya library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The Ekaya library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with the Ekaya library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */
#include <windows.h>
#include <cstdio>
#include <ole2.h>
#include <olectl.h>
#include <assert.h>
#include <msctf.h>

#include "Ekaya.h"
#include "EkayaLangBarButton.h"
#include "EkayaKeyboardFactory.h"
#include "EkayaKeyboard.h"
#include "EkayaEditSession.h"
#include "EkayaInputProcessor.h"
#include "KmflKeyboardFactory.h"
#include "Register.h"
#include "UtfConversion.h"
#include "MessageLogger.h"
#include "resource.h"

using namespace EKAYA_NS;

static const TF_PRESERVEDKEY KEY_ONOFF = { 0x20, TF_MOD_CONTROL };
static const TF_PRESERVEDKEY KEY_NEXT = { 0x20, TF_MOD_CONTROL | TF_MOD_SHIFT };

static const WCHAR ONOFF_DESC[]    = L"OnOff";
static const WCHAR NEXT_DESC[]    = L"Next Keyboard";

#ifdef _DEBUG
// {3A76F650-7784-454a-92F5-33DFFE6A8DDB}
static const GUID GUID_PRESERVEDKEY_ONOFF = 
{ 0x3a76f650, 0x7784, 0x454a, { 0x92, 0xf5, 0x33, 0xdf, 0xfe, 0x6a, 0x8d, 0xdb } };

// {8DD8D792-29A1-42a9-A86B-7E70DAB3EE0C}
static const GUID GUID_PRESERVEDKEY_NEXT = 
{ 0x8dd8d792, 0x29a1, 0x42a9, { 0xa8, 0x6b, 0x7e, 0x70, 0xda, 0xb3, 0xee, 0xc } };

#else
// {7963550C-192E-41e0-A7B0-898881899F1F}
static const GUID GUID_PRESERVEDKEY_ONOFF = 
{ 0x7963550c, 0x192e, 0x41e0, { 0xa7, 0xb0, 0x89, 0x88, 0x81, 0x89, 0x9f, 0x1f } };

// {21E71720-C4F5-4e4b-9DA7-E5A9E3D514DC}
static const GUID GUID_PRESERVEDKEY_NEXT = 
{ 0x21e71720, 0xc4f5, 0x4e4b, { 0x9d, 0xa7, 0xe5, 0xa9, 0xe3, 0xd5, 0x14, 0xdc } };
#endif

const std::string EkayaInputProcessor::EKAYA_DIR = "\\ThanLwinSoft.org\\Ekaya";

#ifdef _M_AMD64
const std::wstring EkayaInputProcessor::EKAYA_INSTALL_REGKEY = L"Ekaya_x86_64";
#endif
#ifdef _M_IX86
const std::wstring EkayaInputProcessor::EKAYA_INSTALL_REGKEY = L"Ekaya_x86";
#endif

const std::wstring EkayaInputProcessor::LIB_NAME = std::wstring(L"Ekaya");
const std::wstring EkayaInputProcessor::ORGANISATION = std::wstring(L"ThanLwinSoft.org");
const std::string EkayaInputProcessor::CONFIG_ACTIVE_KEYBOARD = std::string("activeKeyboard");
const std::string EkayaInputProcessor::CONFIG_LOGGING = std::string("logging");

EkayaInputProcessor::EkayaInputProcessor()
: mOpen(true), mDisabled(true), mRawCodes(true), mRefCount(1), mActiveKeyboard(-1),
  mClientId(TF_CLIENTID_NULL),
  mThreadEventCookie(TF_INVALID_COOKIE), mEditEventCookie(TF_INVALID_COOKIE),
  mMouseCookie(TF_INVALID_COOKIE), mTextLayoutCookie(TF_INVALID_COOKIE),
  mContextOwnerCookie(TF_INVALID_COOKIE),
  mGdiToken(NULL),
  mpThreadMgr(NULL), mpLangBarButton(NULL), mExpectDummyKey(false),
  mpComposition(NULL), mpCompositionRange(NULL), mLogger(NULL)
{
	DllAddRef();
    int logging = getConfigValue(CONFIG_LOGGING, 0);
    
    if (logging == 0)
        setConfigValue(CONFIG_LOGGING, 0);
    if (logging)
    {
        // create a logging directory under APPDATA
        char* appDir = NULL;
        size_t requiredSize;
        getenv_s( &requiredSize, NULL, 0, "APPDATA");
	    appDir = new char[requiredSize];
	    if (appDir)
        {
	        std::string basePath;
		    getenv_s( &requiredSize, appDir, requiredSize, "APPDATA" );
		    basePath = std::string(appDir) + "\\ThanLwinSoft.org";
            //mkdir(basePath);
            // ignore return since it may already exist
            CreateDirectoryA(basePath.c_str(), NULL);
            basePath += "\\ekaya.log";
            mLogger = new MessageLogger(basePath.c_str());
            delete appDir;
            MessageLogger::logMessage("Logging to %s\n", basePath.c_str());
	    }
    }
	mKeyboardFactories.push_back(new KmflKeyboardFactory());
	initKeyboards();
	AddLanguageProfiles(NULL);// in case some new languages were added
}

EkayaInputProcessor::~EkayaInputProcessor()
{
	std::vector<EkayaKeyboardFactory*>::iterator i = mKeyboardFactories.begin();
	while (i != mKeyboardFactories.end())
	{
		delete (*i);
		++i;
	}
    if (mLogger)
    {
        delete mLogger;
        mLogger = NULL;
    }
	DllRelease();
}

// IUnknown
STDAPI EkayaInputProcessor::QueryInterface(REFIID riid, void **ppvObj)
{
    MessageLogger::logMessage("EkayaInputProcessor::QueryInterface %x-%x-%x-%x\n",
        riid.Data1, riid.Data2, riid.Data3, riid.Data4);
	if (ppvObj == NULL)
        return E_INVALIDARG;

    *ppvObj = NULL;

    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfTextInputProcessor))
    {
        *ppvObj = (ITfTextInputProcessor *)this;
    }
    else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    {
        *ppvObj = (ITfThreadMgrEventSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfTextEditSink))
    {
        *ppvObj = (ITfTextEditSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    {
        *ppvObj = (ITfKeyEventSink *)this;
    }
	else if (IsEqualIID(riid, IID_ITfCompositionSink))
    {
        *ppvObj = (ITfCompositionSink *)this;
    }
	else if (IsEqualIID(riid, IID_ITfContextOwnerCompositionSink))
	{
		*ppvObj = (ITfContextOwnerCompositionSink*)this;
	}
	else if (IsEqualIID(riid, IID_ITfTextLayoutSink))
	{
		*ppvObj = (ITfTextLayoutSink*)this;
	}
	else if (IsEqualIID(riid, IID_ITfMouseSink))
    {
        *ppvObj = (ITfMouseSink *)this;
    }

    if (*ppvObj)
    {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDAPI_(ULONG) EkayaInputProcessor::AddRef(void)
{
	++mRefCount;
	return mRefCount;
}

STDAPI_(ULONG) EkayaInputProcessor::Release(void)
{
	unsigned long count = --mRefCount;
	if (count == 0)
		delete this;
	return count;
}

// ITfTextInputProcessor
STDAPI EkayaInputProcessor::Activate(ITfThreadMgr *pThreadMgr, TfClientId tfClientId)
{
	mpThreadMgr = pThreadMgr;
    mpThreadMgr->AddRef();
    mClientId = tfClientId;

	HRESULT status = S_OK;

	ITfSource *pSource;
    if (mpThreadMgr->QueryInterface(IID_ITfSource, (void **)&pSource) == S_OK)
	{
        if (pSource->AdviseSink(IID_ITfThreadMgrEventSink, (ITfThreadMgrEventSink *)this, &mThreadEventCookie) != S_OK)
		{
			//Don't try to Unadvise cookie later
			mThreadEventCookie = TF_INVALID_COOKIE;
			status = E_FAIL;
		}
		//if (pSource->AdviseSink(IID_ITfTextLayoutSink, (ITfTextLayoutSink *)this, &mTextLayoutCookie) != S_OK)
		//{
		//	//Don't try to Unadvise cookie later
		//	mTextLayoutCookie = TF_INVALID_COOKIE;
		//	status = E_FAIL;
		//}
	}
	else status = E_FAIL;
    pSource->Release();

	// GDIPlus
	Gdiplus::GdiplusStartupInput gdiInput;
	Gdiplus::GdiplusStartupOutput gdiOutput;
	if (	Gdiplus::GdiplusStartup(&mGdiToken, &gdiInput, &gdiOutput) != S_OK)
	{
		MessageLogger::logMessage("Failed to start GDIPlus");
	}

	// Document and Keystroke manager
	ITfDocumentMgr *pDocMgrFocus;
    if (status == S_OK &&
		(mpThreadMgr->GetFocus(&pDocMgrFocus) == S_OK) &&
        (pDocMgrFocus != NULL))
    {
        setTextEditSink(pDocMgrFocus);
        pDocMgrFocus->Release();
    }

	ITfKeystrokeMgr *pKeystrokeMgr;
	if (status == S_OK &&
		mpThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void **)&pKeystrokeMgr) == S_OK)
	{
		status = pKeystrokeMgr->AdviseKeyEventSink(mClientId, (ITfKeyEventSink *)this, TRUE);
		status = pKeystrokeMgr->PreserveKey(mClientId, 
                                GUID_PRESERVEDKEY_ONOFF,
                                &KEY_ONOFF,
                                ONOFF_DESC,
                                (ULONG)wcslen(ONOFF_DESC));
		status = pKeystrokeMgr->PreserveKey(mClientId, 
                                GUID_PRESERVEDKEY_NEXT,
                                &KEY_NEXT,
                                NEXT_DESC,
                                (ULONG)wcslen(NEXT_DESC));

		pKeystrokeMgr->Release();
	}
	else status = E_FAIL;

	

	// language bar
	ITfLangBarItemMgr *pLangBarItemMgr;
    if (status == S_OK &&
		mpThreadMgr->QueryInterface(IID_ITfLangBarItemMgr, (void **)&pLangBarItemMgr) == S_OK)
	{
	    if ((mpLangBarButton = new EkayaLangBarButton(this)) != NULL)
		{
			if (pLangBarItemMgr->AddItem(mpLangBarButton) != S_OK)
			{
				mpLangBarButton->Release();
				mpLangBarButton = NULL;
				status = E_FAIL;
			}
			else mpLangBarButton->AddRef();
		}
	    pLangBarItemMgr->Release();
	}
	else status = E_FAIL;

	if (status != S_OK)
	{
		Deactivate(); // cleanup any half-finished init
	}
	else
	{
		mOpen = true;
	}
    return status;
}

STDAPI EkayaInputProcessor::Deactivate()
{
	MessageLogger::logMessage("EkayaInputProcessor::Deactivate\n");
	ITfSource *pSource;
	
	if (mpThreadMgr->QueryInterface(IID_ITfSource, (void **)&pSource) == S_OK)
	{
		if (mThreadEventCookie != TF_INVALID_COOKIE)
		{
			pSource->UnadviseSink(mThreadEventCookie);
		}
		if (mTextLayoutCookie != TF_INVALID_COOKIE)
		{
			pSource->UnadviseSink(mTextLayoutCookie);
		}
		if (mContextOwnerCookie != TF_INVALID_COOKIE)
		{
			pSource->UnadviseSink(mContextOwnerCookie);	
		}
		pSource->Release();
	}
    mThreadEventCookie = TF_INVALID_COOKIE;
	mTextLayoutCookie = TF_INVALID_COOKIE;
	mContextOwnerCookie = TF_INVALID_COOKIE;

	if (mMouseCookie != TF_INVALID_COOKIE)
	{
		ITfMouseTracker * pMouseTracker;
		if (mpThreadMgr->QueryInterface(IID_ITfMouseTracker, (void **)&pMouseTracker) == S_OK)
		{
			pMouseTracker->UnadviseMouseSink(mMouseCookie);
			pMouseTracker->Release();
		}
		mMouseCookie = TF_INVALID_COOKIE;
	}

	setTextEditSink(NULL);

	ITfKeystrokeMgr *pKeystrokeMgr;
    if (mpThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void **)&pKeystrokeMgr) == S_OK)
	{
		pKeystrokeMgr->UnadviseKeyEventSink(mClientId);
		pKeystrokeMgr->UnpreserveKey(GUID_PRESERVEDKEY_ONOFF, &KEY_ONOFF);
		pKeystrokeMgr->UnpreserveKey(GUID_PRESERVEDKEY_NEXT, &KEY_NEXT);
		pKeystrokeMgr->Release();
	}

	// language bar
	ITfLangBarItemMgr *pLangBarItemMgr;
	if (mpLangBarButton != NULL &&
		mpThreadMgr->QueryInterface(IID_ITfLangBarItemMgr, (void **)&pLangBarItemMgr) == S_OK)
    {
        pLangBarItemMgr->RemoveItem(mpLangBarButton);
	    mpLangBarButton->Release();
		mpLangBarButton = NULL;
        pLangBarItemMgr->Release();
    }

	if (mGdiToken) Gdiplus::GdiplusShutdown(mGdiToken);
	mGdiToken = NULL;

    mClientId = TF_CLIENTID_NULL;
	mOpen = false;
    return S_OK;
}

HRESULT EkayaInputProcessor::setTextEditSink(ITfDocumentMgr *pDocMgrFocus)
{
	ITfSource *pSource;
    HRESULT fRet;

    // clear out any previous sink first
    if (mEditEventCookie != TF_INVALID_COOKIE)
    {
        if (mpTextEditSinkContext &&
			mpTextEditSinkContext->QueryInterface(IID_ITfSource, (void **)&pSource) == S_OK)
        {
            pSource->UnadviseSink(mEditEventCookie);
            pSource->Release();
        }

        mpTextEditSinkContext->Release();
        mpTextEditSinkContext = NULL;
        mEditEventCookie = TF_INVALID_COOKIE;
    }

    if (pDocMgrFocus == NULL)
        return S_OK; // caller just wanted to clear the previous sink

    // setup a new sink advised to the topmost context of the document
    if (pDocMgrFocus->GetTop(&mpTextEditSinkContext) != S_OK)
        return E_FAIL;

    if (mpTextEditSinkContext == NULL)
        return S_OK; // empty document, no sink possible

    fRet = E_FAIL;

    if (mpTextEditSinkContext)
	{
		if (mpTextEditSinkContext->QueryInterface(IID_ITfSource, (void **)&pSource) == S_OK)
		{
			if (pSource->AdviseSink(IID_ITfTextEditSink, (ITfTextEditSink *)this, &mEditEventCookie) == S_OK)
			{
				fRet = S_OK;
			}
			else
			{
				mEditEventCookie = TF_INVALID_COOKIE;
			}
			if (mTextLayoutCookie != TF_INVALID_COOKIE ||
				pSource->AdviseSink(IID_ITfTextLayoutSink, (ITfTextLayoutSink *)this, &mTextLayoutCookie) == S_OK)
			{
				fRet = S_OK;
			}
			else
			{
				mTextLayoutCookie = TF_INVALID_COOKIE;
			}
			if (pSource->AdviseSink(IID_ITfContextOwnerCompositionSink, (ITfContextOwnerCompositionSink*)this, &mContextOwnerCookie) == S_OK)
			{
				fRet = S_OK;
			}
			else
			{
				mContextOwnerCookie = TF_INVALID_COOKIE;
			}
			pSource->Release();
		}
	}

    if (fRet != S_OK)
    {
        mpTextEditSinkContext->Release();
        mpTextEditSinkContext = NULL;
    }
	return fRet;
}

// ITfThreadMgrEventSink
STDAPI EkayaInputProcessor::OnInitDocumentMgr(ITfDocumentMgr *pDocMgr)
{
	MessageLogger::logMessage("InitDocumentMgr %lu\n", pDocMgr);
	return S_OK;
}

STDAPI EkayaInputProcessor::OnUninitDocumentMgr(ITfDocumentMgr *pDocMgr)
{
	MessageLogger::logMessage("UninitDocumentMgr %lx\n", pDocMgr);
	return S_OK;
}

STDAPI EkayaInputProcessor::OnSetFocus(ITfDocumentMgr *pDocMgrFocus, ITfDocumentMgr * /*pDocMgrPrevFocus*/)
{
	MessageLogger::logMessage("SetFocus %lx\n", pDocMgrFocus);
	return setTextEditSink(pDocMgrFocus);
}

STDAPI EkayaInputProcessor::OnPushContext(ITfContext *pContext)		
{
	MessageLogger::logMessage("PushContext %lx\n", pContext);
	EkayaSetContextSession * contextSession = new EkayaSetContextSession(this, pContext);
	HRESULT hr = S_OK;
	hr = pContext->RequestEditSession(mClientId, contextSession, TF_ES_SYNC | TF_ES_READ, &hr);
	contextSession->Release();
	// reset context
	mContext = L"";
	mPendingData = L"";
	mPendingDelete = 0;
	return S_OK;
}

STDAPI EkayaInputProcessor::OnPopContext(ITfContext *pContext)
{
	MessageLogger::logMessage("PopContext %lx\n", pContext);
	EkayaSetContextSession * contextSession = new EkayaSetContextSession(this, pContext);
	HRESULT hr = S_OK;
	hr = pContext->RequestEditSession(mClientId, contextSession, TF_ES_SYNC | TF_ES_READ, &hr);
	contextSession->Release();
	// reset context
	mContext = L"";
	mPendingData = L"";
	mPendingDelete = 0;
	return S_OK;
}

// ITfTextEditSink
STDAPI EkayaInputProcessor::OnEndEdit(ITfContext *pContext, TfEditCookie ecReadOnly, ITfEditRecord *pEditRecord)
{
	BOOL fSelectionChanged;
    //IEnumTfRanges *pEnumTextChanges;
    //ITfRange *pRange;
	MessageLogger::logMessage("EndEdit ");
    //
    // did the selection change?
    // The selection change includes the movement of caret as well. 
    // The caret position is represented as an empty selection range when
    // there is no selection.
    //
    if (pEditRecord->GetSelectionStatus(&fSelectionChanged) == S_OK &&
        fSelectionChanged)
    {
		MessageLogger::logMessage("Selection changed\n");
		TF_SELECTION tfSelection;
		ULONG cFetched;
        // If we have pending data then the ranges are unchangeable and we may be mid-delete so don't reset the
        // context. Otherwise, try to get the new context.
        if (mPendingDelete == 0 && mPendingData.length() == 0 &&
            pContext->GetSelection(ecReadOnly, TF_DEFAULT_SELECTION, 1, &tfSelection, &cFetched) == S_OK && cFetched > 0)
		{
            MessageLogger::logMessage("Got selection\n");
			wchar_t buffer[128];
			tfSelection.range->GetText(ecReadOnly, 0, buffer, 128, &cFetched);
			if (cFetched > 0)
			{
				for (size_t i = 0; i < cFetched; i++)
					MessageLogger::logMessage("%x ", buffer[i]);
				MessageLogger::logMessage("\nSelection has %d characters\n", (int)cFetched);
			}
			ITfRange * contextRange = NULL;
			if (tfSelection.range->Clone(&contextRange) == S_OK)
			{
				ITfRange * docStart = NULL;
				pContext->GetStart(ecReadOnly, &docStart);
				if (docStart)
				{
					TF_HALTCOND halt;
					halt.pHaltRange = docStart;
					halt.aHaltPos = TF_ANCHOR_START;
					halt.dwFlags = 0;
					long shiftSize = 0;
					// for the context, we only want what is in front of selection
					contextRange->Collapse(ecReadOnly, TF_ANCHOR_START);
					// try to shift to update context
					if (contextRange->ShiftStart(ecReadOnly, -MAX_CONTEXT, &shiftSize, &halt) == S_OK && shiftSize < 0)
					{
						wchar_t contextBuf[MAX_CONTEXT];
						ULONG contextLength = 0;
						if (contextRange->GetText(ecReadOnly, 0, contextBuf, MAX_CONTEXT, &contextLength) == S_OK &&
							contextLength > 0)
						{
                            bool useNewContext = true;
                            if (contextLength < mContext.length())
                            {
                                useNewContext = false;
                                // if the new context matches the old context, but is shorter, we keep the old context
                                // this happens in notepad for example
                                for (int i = static_cast<int>(mContext.length() - 1),
									j = contextLength - 1; i >= 0 && j >= 0; i--, j--)
                                {
                                    if (mContext[i] != contextBuf[j])
                                    {
                                        useNewContext = true;
                                        break;
                                    }
                                }
                            }
                            if (useNewContext)
                            {
		                        mPendingData = L"";
		                        mPendingDelete = 0;
                                mContext.assign(std::wstring(contextBuf, contextLength));
							    for (size_t i = 0; i < contextLength; i++)
								    MessageLogger::logMessage("%x ", contextBuf[i]);
							    MessageLogger::logMessage("\n");
                            }
                            else
                            {
                                MessageLogger::logMessage("keeping old context since new length is %d not %d\n",
                                    (int)contextLength, (int)mContext.length());
                            }
						}
					}
					docStart->Release();
				}
				contextRange->Release();
			}

			tfSelection.range->Release();
		}
		/*
		const GUID * props[16];
		IEnumTfRanges * pRanges = NULL;
		HRESULT hr = pEditRecord->GetTextAndPropertyUpdates(0, props, 16, &pRanges);
		if (hr == S_OK && pRanges != NULL)
		{
			ITfRange * ranges[16];
			ULONG fetched = 0;
			pRanges->Next(16, ranges, &fetched);
			MessageLogger::logMessage("%d ranges\n", (int)fetched);
		}
		*/
    }
	else
	{
		MessageLogger::logMessage("no selection change\n");
	}

    // text modification?
	/*
    if (pEditRecord->GetTextAndPropertyUpdates(TF_GTP_INCL_TEXT, NULL, 0, &pEnumTextChanges) == S_OK)
    {
        if (pEnumTextChanges->Next(1, &pRange, NULL) == S_OK)
        {
            // pRange is the updated range.
			MessageLogger::logMessage("Range changed\n");
            pRange->Release();
        }

        pEnumTextChanges->Release();
    }
	*/
	return S_OK;
}

// ITfKeyEventSink
STDAPI EkayaInputProcessor::OnSetFocus(BOOL fForeground)
{
	MessageLogger::logMessage("SetFocus %d\n", fForeground);
	return S_OK;
}


STDAPI EkayaInputProcessor::OnStartComposition( 
            /* [in] */ __RPC__in_opt ITfCompositionView * /*pComposition*/,
            /* [out] */ __RPC__out BOOL * /*pfOk*/)
{
	MessageLogger::logMessage("StartComposition");
	return S_OK;
}
        
STDAPI EkayaInputProcessor::OnUpdateComposition( 
            /* [in] */ __RPC__in_opt ITfCompositionView * /*pComposition*/,
            /* [in] */ __RPC__in_opt ITfRange * /*pRangeNew*/)
{
	MessageLogger::logMessage("UpdateComposition");
	return S_OK;
}
        
STDAPI EkayaInputProcessor::OnEndComposition( 
            /* [in] */ __RPC__in_opt ITfCompositionView * /*pComposition*/)
{
	MessageLogger::logMessage("EndComposition");
	return S_OK;
}

/**
* Test whether the specified key is a special character that should be ignored
*/
bool EkayaInputProcessor::ignoreKey(WPARAM wParam)
{
	if (wParam < 0x30) // < '0'
	{
		// need to process delete when we didn't send it, since it may need special context handling
		if (wParam == VK_BACK)
		{
			if (mPendingDelete == 0 && mContext.length() > 0)
				return false;
			return true;
		}
		if (wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_SPACE)
			return true;
		return false;
	}
	else if (wParam < 0x5B) // 0x41 - 0x5A A-Z
	{
		return false;
	}
	else if (wParam >= 0xBA && wParam < 0xE0) // OEM keys
	{
		return false;
	}
	// ignore others
	return true;
}

STDAPI EkayaInputProcessor::OnTestKeyDown(ITfContext * /*pContext*/, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
	MessageLogger::logMessage("OnTestKeyDown %x %x\n", (int)wParam, (int)lParam);
	HRESULT hr = S_OK;
	if (!isKeyboardOpen() || isKeyboardDisabled())
	{
		*pfEaten = FALSE;
		MessageLogger::logMessage("keyboard disabled\n");
		return hr;
	}
	else
	{
		if (ignoreKey(wParam)) 
		{
			*pfEaten = FALSE;
		}
		// ignore ctrl keys
		else if (mKeyState.at(KEY_CTRL) && (wParam != VK_CONTROL))
		{
			*pfEaten = FALSE;
		}
		else
		{
			*pfEaten = TRUE;
		}
	}
	switch (wParam)
	{
	case VK_HOME:
	case VK_END:
	case VK_LEFT:
	case VK_UP:
	case VK_RIGHT:
	case VK_DOWN:
	case VK_RETURN:
		// reset context
		mContext = L"";
		mPendingData = L"";
		mPendingDelete = 0;
		break;
	}
	if (wParam == VK_BACK)// 0x08
	{
		int bkspCount = (lParam & 0xff);
		if (mPendingDelete > 0)
		{
			mPendingDelete -= bkspCount;
			mExpectDummyKey = true;
			if (mPendingDelete <= 0)
			{
				// Request append
				MessageLogger::logMessage("Pending Delete finished\n");
				mPendingDelete = 0;
			}
		}
		else
		{
			// backspace is now past through in case it needs context specific handling,
			// so don't need to adjust context here
			//bkspCount = min(bkspCount, static_cast<int>(mContext.length()) );
			//mContext.erase(mContext.length() - bkspCount, bkspCount);
		}
		MessageLogger::logMessage("Pending %d\n", mPendingDelete);
	}
	return hr;
}

STDMETHODIMP EkayaInputProcessor::OnKeyDown(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
	if (!isKeyboardOpen() || isKeyboardDisabled())
	{
		*pfEaten = FALSE;
		return S_OK;
	}
	else
	{
		*pfEaten = TRUE;
	}

	EkayaEditSession *pEditSession = NULL;
    HRESULT hr = E_FAIL;
	MessageLogger::logMessage("OnKeyDown %x %x\n", (int)wParam, (int)lParam);
	// check for control keys
	if (wParam < 0x30)
	{
		switch (wParam)
		{
		case VK_SHIFT:
			mKeyState.set(KEY_SHIFT);
			return S_OK;
		case VK_CONTROL:
			mKeyState.set(KEY_CTRL);
			return S_OK;
		case VK_ESCAPE:
		case VK_SPACE:
			break;
		case VK_BACK:
			break;
		default:
			// ignore
			*pfEaten = FALSE;
			return S_OK;
		}
		
	}

	if (mRawCodes)
	{
		if (!mKeyState.at(KEY_SHIFT))
		{
			if (wParam > 0x40 && wParam < 0x5B)
			{
				// allow for lower case
				wParam += 0x20;
			}
			else
			{
				switch (wParam)
				{
				case VK_BACK:
					wParam = 0xFF08;// backspace code used by kmfl
					break;
				case VK_OEM_1:// 0xBA ';:' for US
					wParam = 0x3B;
					break;
				case VK_OEM_PLUS: // 0xBB   // '+' any country
					wParam = 0x3D;
					break;
				case VK_OEM_COMMA: // 0xBC   // ',' any country
					wParam = 0x2C;
					break;
				case VK_OEM_MINUS: // 0xBD   // '-' any country
					wParam = 0x2D;
					break;
				case VK_OEM_PERIOD: // 0xBE   // '.' any country
					wParam = 0x2E;
					break;
				case VK_OEM_2: // 0xBF   // '/?' for US
					wParam = 0x2F;
					break;
				case VK_OEM_3: // 0xC0   // '`~' for US
					wParam = 0x60;
					break;
				case VK_OEM_4: // 0xDB  //  '[{' for US
					wParam = 0x5B;
					break;
				case VK_OEM_5: // 0xDC  //  '\|' for US
					wParam = 0x5C;
					break;
				case VK_OEM_6: // 0xDD  //  ']}' for US
					wParam = 0x5D;
					break;
				case VK_OEM_7: // 0xDE  //  ''"' for US
					wParam = 0x27;
					break;
				}
			}
		}
		else
		{
			// shifted
			switch (wParam)
			{
			case VK_BACK:// treat shift backspace same as normal backspace
				wParam = 0xFF08;// backspace code used by kmfl
				break;
			case 0x30:
				wParam = 0x29;//)
				break;
			case 0x31:
				wParam = 0x21;//!
				break;
			case 0x32:
				wParam = 0x40;//@
				break;
			case 0x33:
				wParam = 0x23;//#
				break;
			case 0x34:
				wParam = 0x24;//$
				break;
			case 0x35:
				wParam = 0x25;//%
				break;
			case 0x36:
				wParam = 0x5E;//^
				break;
			case 0x37:
				wParam = 0x26;//&
				break;
			case 0x38:
				wParam = 0x2A;//*
				break;
			case 0x39:
				wParam = 0x28;//(
				break;
			case VK_OEM_1:// 0xBA ';:' for US
				wParam = 0x3A;
				break;
			case VK_OEM_PLUS: // 0xBB   // '+' any country
				wParam = 0x2B;
				break;
			case VK_OEM_COMMA: // 0xBC   // ',<' any country
				wParam = 0x3C;
				break;
			case VK_OEM_MINUS: // 0xBD   // '-' any country
				wParam = 0x5F;
				break;
			case VK_OEM_PERIOD: // 0xBE   // '.>' any country
				wParam = 0x3E;
				break;
			case VK_OEM_2: // 0xBF   // '/?' for US
				wParam = 0x3F;
				break;
			case VK_OEM_3: // 0xC0   // '`~' for US
				wParam = 0x7E;
				break;
			case VK_OEM_4: // 0xDB  //  '[{' for US
				wParam = 0x7B;
				break;
			case VK_OEM_5: // 0xDC  //  '\|' for US
				wParam = 0x7C;
				break;
			case VK_OEM_6: // 0xDD  //  ']}' for US
				wParam = 0x7D;
				break;
			case VK_OEM_7: // 0xDE  //  ''"' for US
				wParam = 0x22;
				break;
			}
		}
		if (wParam == 0xFF08)
		{
			// eat if we are expecting the dummy key instead, to avoid muliple deletes
			if (mExpectDummyKey)
			{
				MessageLogger::logMessage("Eaten delete while waiting for dummy\n");
				*pfEaten = TRUE;
				return S_OK;
			}
			// check whether this is a delete sent by us, if the app hasn't called 
			// OnTestKeyDown e.g. Publisher
			// also pass through if there is no context available
			if((mPendingDelete > 0) || (mContext.length() == 0))
			{
				wParam = VK_BACK;
				int bkspCount = (lParam & 0xff);
				if (mPendingDelete > 0)
				{
					mPendingDelete -= bkspCount;
					if (mPendingDelete < 0) mPendingDelete = 0;
					mExpectDummyKey = true;
				}
				
				*pfEaten = FALSE;
				MessageLogger::logMessage("Received %d deletes, which we sent. Now %d pending.\n", bkspCount, mPendingDelete);
				return S_OK;
			}
		}
		// the dummy key is sent by us during a delete to keep track of progress
		if (wParam == DUMMY_KEY)
		{
			mExpectDummyKey = false;
			if (mPendingDelete > 0)
			{
				INPUT delInput[2];
				delInput[0].type = INPUT_KEYBOARD;
				delInput[0].ki.wVk = VK_BACK;
				delInput[0].ki.wScan = 0;
				delInput[0].ki.dwExtraInfo = 0;
				delInput[0].ki.dwFlags = 0;
				delInput[0].ki.time = 0;
				delInput[1].type = INPUT_KEYBOARD;
				delInput[1].ki.wVk = DUMMY_KEY;
				delInput[1].ki.wScan = 0;
				delInput[1].ki.dwExtraInfo = 0;
				delInput[1].ki.dwFlags = 0;
				delInput[1].ki.time = 0;
				SendInput(2, delInput, sizeof(INPUT));
				return S_OK;
			}
			else if (mPendingData.length() > 0)
			{
				pEditSession = new EkayaEditSession(this, pContext, mPendingData);
				mPendingData = L"";
			}
			else
			{
				// nothing to do
				return S_OK;
			}
		}
		else
		{
			// it is a normal key, reset, dummy key may have been lost somehow
			mExpectDummyKey = false;
			// we'll insert a char ourselves in place of this keystroke
			if ((pEditSession = new EkayaEditSession(this, pContext, wParam)) == NULL)
				return E_FAIL;
		}
	}
	else // !mRawCodes
	{
		// should we use ToUnicodeEx?
		BYTE keyState = 0;
		const int BUFFER_LEN = 4;
		wchar_t uniBuffer[BUFFER_LEN];
		// currently doesn't work, perhaps because the keystate is wrong
		int uCount = ToUnicode(static_cast<UINT>(wParam), static_cast<UINT>(lParam), &keyState, uniBuffer, BUFFER_LEN, 0);
		if (uCount <= 0)
		{
			*pfEaten = FALSE;
			return S_OK;
		}
		// TODO handle > 1 character case
		if ((pEditSession = new EkayaEditSession(this, pContext, uniBuffer[0])) == NULL)
			return E_FAIL;
	}
    
	ITfContextView * contextView;
	pContext->GetActiveView(&contextView);
	HWND hWindow;
	contextView->GetWnd(&hWindow);
	contextView->Release();
	MessageLogger::logMessage("Window:%lx %lx\n", hWindow, GetActiveWindow());

	if (pEditSession)
	{
		// A lock is required, we want it to be synchronous so we know the exact order
		hr = pContext->RequestEditSession(mClientId, pEditSession, TF_ES_SYNC | TF_ES_READWRITE, &hr);
		pEditSession->Release();
	}
	else
	{
		MessageLogger::logMessage("No EditSession!\n");
	}
	return hr;
}

STDMETHODIMP EkayaInputProcessor::OnTestKeyUp(ITfContext * /*pContext*/, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
	MessageLogger::logMessage("OnTestKeyUp %x %x\n", (int)wParam, (int)lParam);
	if (!isKeyboardOpen() || isKeyboardDisabled())
	{
		*pfEaten = FALSE;
	}
	else
	{
		if (ignoreKey(wParam)) 
		{
			*pfEaten = FALSE;
		}
		// ignore ctrl keys
		else if (mKeyState.at(KEY_CTRL) && (wParam != VK_CONTROL))
		{
			*pfEaten = FALSE;
		}
		else
		{
			*pfEaten = TRUE;
		}
	}
	return S_OK;
}

STDMETHODIMP EkayaInputProcessor::OnKeyUp(ITfContext * /*pContext*/, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
	MessageLogger::logMessage("OnKeyUp %x %x\n", (int)wParam, (int)lParam);
	// check for control keys
	if (wParam < 0x30)
	{
		switch (wParam)
		{
		case VK_SHIFT:
			mKeyState.set(KEY_SHIFT, false);
			break;
		case VK_CONTROL:
			mKeyState.set(KEY_CTRL, false);
			break;
		case VK_SPACE:
			break;
		default:
			*pfEaten = TRUE;
			return S_OK;
		}
		
	}
	if (!isKeyboardOpen() || isKeyboardDisabled())
	{
		*pfEaten = FALSE;
	}
	else
	{
		*pfEaten = TRUE;
	}
	return S_OK;
}

STDMETHODIMP EkayaInputProcessor::OnPreservedKey(ITfContext * /*pContext*/, REFGUID rguid, BOOL *pfEaten)
{
	MessageLogger::logMessage("OnPreservedKey\n");
	if (IsEqualGUID(rguid, GUID_PRESERVEDKEY_ONOFF))
    {
        BOOL fOpen = isKeyboardOpen();
        setKeyboardOpen(fOpen ? FALSE : TRUE);
		setActiveKeyboard(mActiveKeyboard);
        *pfEaten = TRUE;
    }
	else if (IsEqualGUID(rguid, GUID_PRESERVEDKEY_NEXT))
    {
		int next = mActiveKeyboard + 1;
		if (next >= static_cast<int>(mKeyboards.size()))
			next = 0;
		setKeyboardOpen(true);
		setActiveKeyboard(next);
		*pfEaten = TRUE;
    }
    else
    {
        *pfEaten = FALSE;
    }
	return S_OK;
}

//+---------------------------------------------------------------------------
//
// OnCompositionTerminated
//
// Callback for ITfCompositionSink.  The system calls this method whenever
// someone other than this service ends a composition.
//----------------------------------------------------------------------------

STDAPI EkayaInputProcessor::OnCompositionTerminated(TfEditCookie /*ecWrite*/, ITfComposition * /*pComposition*/)
{

    // releae our cached composition
    if (mpComposition != NULL)
    {
		if (mpCompositionRange)
		{
			mpCompositionRange->Release();
			mpCompositionRange = NULL;
		}
        mpComposition->Release();
        mpComposition = NULL;
    }

    return S_OK;
}

STDAPI EkayaInputProcessor::OnMouseEvent(ULONG /*uEdge*/,
                         ULONG /*uQuadrant*/,
                         DWORD /*dwBtnStatus*/,
                         BOOL * /* pfEaten*/)
{
	return S_OK;
}

// CClassFactory factory callback
HRESULT EkayaInputProcessor::CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppvObj)
{
	EkayaInputProcessor *pCase;
    HRESULT hr;
    MessageLogger::logMessage("CreateInstance %x-%x-%x-%x outer %x\n", riid.Data1,
        riid.Data2, riid.Data3, riid.Data4, pUnkOuter);

    if (ppvObj == NULL)
        return E_INVALIDARG;

    *ppvObj = NULL;

    if (NULL != pUnkOuter)
        return CLASS_E_NOAGGREGATION;

    if ((pCase = new EkayaInputProcessor) == NULL)
        return E_OUTOFMEMORY;

    hr = pCase->QueryInterface(riid, ppvObj);

	if (hr == S_OK)
	{
		pCase->Release(); // caller still holds ref if hr == S_OK
	}
    return hr;
}

STDAPI EkayaInputProcessor::OnLayoutChange(ITfContext * /*pContext*/, TfLayoutCode lcode, ITfContextView * /*pContextView*/)
{
	switch (lcode)
	{
	case TF_LC_CHANGE:
		break;
	case TF_LC_DESTROY:
		break;
	default:
		break;
	}
	MessageLogger::logMessage("OnLayoutChange %d\n", lcode);
	return S_OK;
}

// utility function for compartment
bool EkayaInputProcessor::isKeyboardDisabled()
{
	ITfCompartmentMgr *pCompMgr = NULL;
    ITfDocumentMgr *pDocMgrFocus = NULL;
    ITfContext *pContext = NULL;
    mDisabled = false;

    if ((mpThreadMgr->GetFocus(&pDocMgrFocus) != S_OK) ||
        (pDocMgrFocus == NULL))
    {
        // if there is no focus document manager object, the keyboard 
        // is disabled.
        mDisabled = true;
    }

    if (!mDisabled && ((pDocMgrFocus->GetTop(&pContext) != S_OK) ||
        (pContext == NULL)))
    {
        // if there is no context object, the keyboard is disabled.
        mDisabled = true;
    }

    if (!mDisabled && pContext->QueryInterface(IID_ITfCompartmentMgr, (void **)&pCompMgr) == S_OK)
    {
        ITfCompartment *pCompartmentDisabled;
        ITfCompartment *pCompartmentEmptyContext;

        // Check GUID_COMPARTMENT_KEYBOARD_DISABLED.
        if (pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED, &pCompartmentDisabled) == S_OK)
        {
            VARIANT var;
            if (S_OK == pCompartmentDisabled->GetValue(&var))
            {
                if (var.vt == VT_I4) // Even VT_EMPTY, GetValue() can succeed
                {
                    mDisabled = var.lVal? true : false;
                }
            }
            pCompartmentDisabled->Release();
        }

        // Check GUID_COMPARTMENT_EMPTYCONTEXT.
        if (pCompMgr->GetCompartment(GUID_COMPARTMENT_EMPTYCONTEXT, &pCompartmentEmptyContext) == S_OK)
        {
            VARIANT var;
            if (S_OK == pCompartmentEmptyContext->GetValue(&var))
            {
                if (var.vt == VT_I4) // Even VT_EMPTY, GetValue() can succeed
                {
					mDisabled = var.lVal? true : false;
                }
            }
            pCompartmentEmptyContext->Release();
        }

        pCompMgr->Release();
    }

    if (pContext)
        pContext->Release();

    if (pDocMgrFocus)
        pDocMgrFocus->Release();

	return mDisabled;
}

bool EkayaInputProcessor::isKeyboardOpen()
{
	return mOpen;
}

HRESULT EkayaInputProcessor::setKeyboardOpen(bool fOpen)
{
	mOpen = fOpen;
	mKeyState.set(KEY_SHIFT, false);
	mKeyState.set(KEY_CTRL, false);
	// remove any old context, since it is likely to be invalid
	mContext = L"";
	mPendingDelete = 0;
	mPendingData = L"";
	MessageLogger::logMessage("setKeyboardOpen %d\n", fOpen);
	return S_OK;
}

const std::wstring EkayaInputProcessor::getMessage(UINT uid, const wchar_t * defaultMessage)
{
	std::wstring message = defaultMessage;
	try
	{
		const int MAX_MSG_BUFFER = 256;
		wchar_t wBuffer[MAX_MSG_BUFFER];
		int count = LoadStringW(g_hInst, uid, wBuffer, MAX_MSG_BUFFER);
		if (count)
		{
			message = wBuffer;
		}
	}
	catch(...)
	{
		MessageLogger::logMessage("Resource exception");
	}
	return message;
}

void EkayaInputProcessor::setComposition(ITfComposition * composition)
{
	if (mpComposition != composition)
	{
		if (mpComposition)
			mpComposition->Release();
		if (mpCompositionRange)
		{
			mpCompositionRange->Release();
			mpCompositionRange = NULL;
		}
		mpComposition = composition;
		//if (mpComposition && mpComposition->GetRange(&mpCompositionRange) == S_OK)
		//{
		//	
		//	// Mouse
		//	ITfMouseTracker * pMouseTracker;
		//	if (mpThreadMgr->QueryInterface(IID_ITfMouseTracker, (void **)&pMouseTracker) == S_OK)
		//	{
		//		ITfRange * range = NULL;
		//		if (pMouseTracker->AdviseMouseSink(mpCompositionRange, this, &mMouseCookie) != S_OK)
		//		{
		//			MessageLogger::logMessage("AdviseMouseSink failed\n");
		//		}
		//		pMouseTracker->Release();
		//	}
		//}
	}
}

ITfComposition * EkayaInputProcessor::getComposition()
{
	return mpComposition;
};

void EkayaInputProcessor::initKeyboards()
{
	// TODO initialize factories more generically
    std::string installPath = getInstallLocation();
	for (size_t i = 0; i < mKeyboardFactories.size(); i++)
	{
		std::vector <EkayaKeyboard * > keyboards =
            mKeyboardFactories[i]->loadKeyboards(installPath.size()? installPath.c_str() : NULL);
		for (size_t j = 0; j < keyboards.size(); j++)
		{
			if (keyboards[j])
				mKeyboards.push_back(keyboards[j]);
		}
	}
	// TODO remember last active keyboard
	if (mKeyboards.size())
	{
		mActiveKeyboard = getConfigValue(CONFIG_ACTIVE_KEYBOARD, 0);
		if (mActiveKeyboard > static_cast<int>(mKeyboards.size()))
			mActiveKeyboard = 0;
	}
	MessageLogger::logMessage("initKeyboards %d", (int)mKeyboards.size());
}

void EkayaInputProcessor::setActiveKeyboard(int keyboardIndex)
{
	MessageLogger::logMessage("setActiveKeyboard %d", keyboardIndex);
	mActiveKeyboard = keyboardIndex;
	mpLangBarButton->Show(false);
	ITfLangBarItemMgr * pLangBarItemMgr = NULL;
	if (mpThreadMgr->QueryInterface(IID_ITfLangBarItemMgr, (void **)&pLangBarItemMgr) == S_OK)
	{
		if (mpLangBarButton)
		{
			if (pLangBarItemMgr->RemoveItem(mpLangBarButton) == S_OK)
			{
				if (pLangBarItemMgr->AddItem(mpLangBarButton) != S_OK)
				{
					mpLangBarButton->Release();
					mpLangBarButton = NULL;
				}
			}
			mpLangBarButton->Show(true);
		}
	    pLangBarItemMgr->Release();
	}
	setConfigValue(CONFIG_ACTIVE_KEYBOARD, keyboardIndex);
}

/**
* Gets a configuration parameter from the registry
* @param configName name of key
* @param defaultValue value to return if the key isn't found
* @return value of the configuration parameter
*/
std::string EkayaInputProcessor::getInstallLocation()
{
	HKEY hKeySoftware;
	HKEY hKeyOrg;
	HKEY hKeyProgram;
    std::string value;
	if (RegOpenKeyW(HKEY_LOCAL_MACHINE, L"Software", &hKeySoftware) == ERROR_SUCCESS)
	{
		if (RegOpenKeyW(hKeySoftware, (ORGANISATION).c_str(), &hKeyOrg) == ERROR_SUCCESS)
		{
			if (RegOpenKeyW(hKeyOrg, (EKAYA_INSTALL_REGKEY).c_str(), &hKeyProgram) == ERROR_SUCCESS)
			{
				char data[1024];
				DWORD type;
				DWORD bytes = sizeof(data);
				LSTATUS ret = RegQueryValueExA(hKeyProgram, NULL, NULL,
                    &type, reinterpret_cast<BYTE*>(data), &bytes);
				// RegGetValue doesn't seem to work on Windows XP
				//if ((ret = RegGetValueA(hKeyProgram, NULL, configName.c_str(), RRF_RT_REG_SZ, &type, data, &bytes) == ERROR_SUCCESS) &&
                if ((ret == ERROR_SUCCESS) && (type == REG_SZ))
				{
                    data[bytes] = '\0';
					MessageLogger::logMessage("Read value %s\n", data);
                    value = std::string(data);
				}
				else
				{
					MessageLogger::logMessage("Failed to read value (error %d) type %d\n", ret, type);
				}
        		RegCloseKey(hKeyProgram);
			}
            else
            {
                MessageLogger::logMessage(L"Key not found in registry %s\n",
                    EKAYA_INSTALL_REGKEY.c_str());
            }
    		RegCloseKey(hKeyOrg);
		}
        else
        {
            MessageLogger::logMessage("Organisation not found in registry\n");
        }
		RegCloseKey(hKeySoftware);
	}
	return value;
}


/**
* Gets a configuration parameter from the registry
* @param configName name of key
* @param defaultValue value to return if the key isn't found
* @return value of the configuration parameter
*/
int EkayaInputProcessor::getConfigValue(std::string configName, int defaultValue)
{
	HKEY hKeySoftware;
	HKEY hKeyOrg;
	HKEY hKeyProgram;
    int value = defaultValue;
	if (RegOpenKeyW(HKEY_CURRENT_USER, L"Software", &hKeySoftware) == ERROR_SUCCESS)
	{
		if (RegOpenKeyW(hKeySoftware, (ORGANISATION).c_str(), &hKeyOrg) == ERROR_SUCCESS)
		{
			if (RegOpenKeyW(hKeyOrg, (LIB_NAME).c_str(), &hKeyProgram) == ERROR_SUCCESS)
			{
				DWORD dValue = 0;
				DWORD type;
				DWORD bytes = sizeof(dValue);
				LSTATUS ret = RegQueryValueExA(hKeyProgram, configName.c_str(), NULL,
                    &type, reinterpret_cast<BYTE*>(&dValue), &bytes);
				// RegGetValue doesn't seem to work on Windows XP
//				if ((ret = RegGetValueA(hKeyProgram, NULL, configName.c_str(), RRF_RT_REG_DWORD, &type, reinterpret_cast<BYTE*>(&dValue), &bytes) == ERROR_SUCCESS) &&
                if ((ret == ERROR_SUCCESS) && (type == REG_DWORD))
				{
					value = static_cast<int>(dValue);
					MessageLogger::logMessage("Read value %d from %s\n", value, configName.c_str());
				}
				else
				{
					MessageLogger::logMessage("Failed to read value (error %d) from %s\n", ret, configName.c_str());
				}
        		RegCloseKey(hKeyProgram);
			}
    		RegCloseKey(hKeyOrg);
		}
		RegCloseKey(hKeySoftware);
	}
	return value;
}

/**
* Sets a configuration parameter in the registry
* @param configName name of key
* @param value to store
*/
void EkayaInputProcessor::setConfigValue(std::string configName, int value)
{
	HKEY hKeySoftware;
	HKEY hKeyOrg;
	HKEY hKeyProgram;
	DWORD dw;
	std::wstring keyPath = std::wstring(L"Software");
	if (RegOpenKeyW(HKEY_CURRENT_USER, keyPath.c_str(), &hKeySoftware) == ERROR_SUCCESS)
	{
		if (RegOpenKeyW(hKeySoftware, (ORGANISATION).c_str(), &hKeyOrg) != ERROR_SUCCESS)
		{
			if (RegCreateKeyExW(hKeySoftware, (ORGANISATION).c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKeyOrg, &dw)
				!= ERROR_SUCCESS)
			{
			    RegCloseKey(hKeySoftware);
				MessageLogger::logMessage("Failed to set organisation key\n");
				return;
			}
		}
		if (RegOpenKeyW(hKeyOrg, (LIB_NAME).c_str(), &hKeyProgram) != ERROR_SUCCESS)
		{
			if (RegCreateKeyExW(hKeyOrg, (LIB_NAME).c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKeyProgram, &dw)
				!= ERROR_SUCCESS)
			{
        		RegCloseKey(hKeyOrg);
				MessageLogger::logMessage("Failed to set program key\n");
				return;
			}
		}
		DWORD dValue = value;
		if (RegSetValueExA(hKeyProgram, configName.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE*>(&dValue), sizeof(DWORD))
            != ERROR_SUCCESS)
		{
			MessageLogger::logMessage("Failed to set key value\n");
		}
		else
		{
		    // Shouldn't need to flush
			// RegFlushKey(hKeyProgram);
		}
		RegCloseKey(hKeyProgram);
		RegCloseKey(hKeyOrg);
		RegCloseKey(hKeySoftware);
	}
}
