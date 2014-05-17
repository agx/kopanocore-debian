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

#include "platform.h"

#include "ECServerIndexer.h"
#include "ECLogger.h"
#include "ECConfig.h"
#include "ECIndexDB.h"
#include "ECIndexFactory.h"
#include "IStreamAdapter.h"
#include "ECIndexImporter.h"

#include "zarafa-search.h"
#include "pthreadutil.h"
#include "mapi_ptr.h"
#include "CommonUtil.h"
#include "ECRestriction.h"

#include <mapi.h>
#include <mapicode.h>
#include <mapitags.h>
#include <edkmdb.h>

#include <stdio.h>

#include <memory>
#include <set>
#include "mapi_ptr.h"
#include "IECExportChanges.h"

/* HACK!
 *
 * We are using an include from the client since we need to know the structure of
 * the store entryid for HrExtractServerName. Since this is pretty non-standard we're
 * not including this in common.
 *
 * The 'real' way to do this would be to have this function somewhere in provider/client
 * and then use some kind of interface to call it, but that seems rather over-the-top for
 * something we're using once.
 */
#include "../../provider/include/Zarafa.h"

typedef mapi_object_ptr<IECExportChanges, IID_IECExportChanges> ECExportChangesPtr;
//DEFINEMAPIPTR(ECExportChanges);

using namespace std;

#define WSTR(x) (PROP_TYPE((x).ulPropTag) == PT_UNICODE ? (x).Value.lpszW : L"<Unknown>")

// Define ECImportContentsChangePtr
typedef mapi_object_ptr<IECImportContentsChanges, IID_IECImportContentsChanges> ECImportContentsChangesPtr;
//DEFINEMAPIPTR(ECImportContentsChanges);

/**
 * Extract server name from wrapped store entryid
 *
 * Will only return the servername if it is a pseudo URL, otherwise this function fails with
 * MAPI_E_INVALID_PARAMETER. Also only supports v1 zarafa EIDs.
 *
 * @param[in] lpStoreEntryId 	EntryID to extract server name from
 * @param[in] cbEntryId 		Bytes in lpStoreEntryId
 * @param[out] strServerName	Extracted server name
 * @return result
 */
HRESULT HrExtractServerName(LPENTRYID lpStoreEntryId, ULONG cbStoreEntryId, std::string &strServerName)
{
	HRESULT hr = hrSuccess;
	EntryIdPtr lpEntryId;
	ULONG cbEntryId;

	hr = UnWrapStoreEntryID(cbStoreEntryId, lpStoreEntryId, &cbEntryId, &lpEntryId);
	if(hr != hrSuccess)
		goto exit;

	if(((PEID)lpEntryId.get())->ulVersion != 1) {
	    hr = MAPI_E_INVALID_PARAMETER;
		ASSERT(false);
		goto exit;
	}

	strServerName = (char *)((PEID)lpEntryId.get())->szServer;

	// Servername is now eg. pseudo://servername

	if(strServerName.compare(0, 9, "pseudo://") == 0) {
		strServerName.erase(0, 9);
	} else {
		hr = MAPI_E_INVALID_PARAMETER;
		goto exit;
	}

exit:
	return hr;
}

ECServerIndexer::ECServerIndexer(ECConfig *lpConfig, ECLogger *lpLogger, ECThreadData *lpThreadData)
{
    m_lpConfig = lpConfig;
    m_lpLogger = lpLogger;
    m_lpThreadData = lpThreadData;

    pthread_mutex_init(&m_mutexExit, NULL);
    pthread_cond_init(&m_condExit, NULL);
    m_bExit = false;
	m_bFast = false;
    m_bThreadStarted = false;

    pthread_mutex_init(&m_mutexTrack, NULL);
    pthread_cond_init(&m_condTrack, NULL);
    m_ulTrack = 0;

	pthread_mutex_init(&m_mutexRebuild, NULL);
	pthread_cond_init(&m_condRebuild, NULL);

    m_guidServer = GUID_NULL;

    m_ulIndexerState = 0;
}

ECServerIndexer::~ECServerIndexer()
{
    pthread_mutex_lock(&m_mutexExit);
    m_bExit = true;
    pthread_cond_signal(&m_condExit);
    pthread_mutex_unlock(&m_mutexExit);

    pthread_mutex_lock(&m_mutexRebuild);
    pthread_cond_signal(&m_condRebuild);
    pthread_mutex_unlock(&m_mutexRebuild);

    pthread_join(m_thread, NULL);
    pthread_join(m_threadRebuild, NULL);
}

HRESULT ECServerIndexer::Create(ECConfig *lpConfig, ECLogger *lpLogger, ECThreadData *lpThreadData, ECServerIndexer **lppIndexer)
{
    HRESULT hr = hrSuccess;
    ECServerIndexer *lpIndexer = new ECServerIndexer(lpConfig, lpLogger, lpThreadData);

    lpIndexer->AddRef();

    hr = lpIndexer->Start();
    if(hr != hrSuccess)
        goto exit;

    *lppIndexer = lpIndexer;
exit:
    if(hr != hrSuccess && lpIndexer)
        lpIndexer->Release();

    return hr;
}

/**
 * Start the indexer thread
 */
HRESULT ECServerIndexer::Start()
{
    HRESULT hr = hrSuccess;

    if(pthread_create(&m_thread, NULL, ECServerIndexer::ThreadEntry, this) != 0) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to start thread: %s", strerror(errno));
        hr = MAPI_E_CALL_FAILED;
        goto exit;
    }

    if(pthread_create(&m_threadRebuild, NULL, ECServerIndexer::ThreadRebuildEntry, this) != 0) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to start thread: %s", strerror(errno));
        hr = MAPI_E_CALL_FAILED;
        goto exit;
    }

    m_bThreadStarted = true;

exit:
    return hr;
}

/**
 * Thread entry point delegate
 */
void *ECServerIndexer::ThreadEntry(void *lpParam)
{
    ECServerIndexer *lpIndexer = (ECServerIndexer *)lpParam;

    lpIndexer->Thread();
	return NULL;
}

/**
 * This is the main work thread that is started for the ECServerIndexer object
 *
 * Lifetime of the thread is close to that of the entire indexer process
 */
HRESULT ECServerIndexer::Thread()
{
    HRESULT hr = hrSuccess;

    m_lpLogger->Log(EC_LOGLEVEL_DEBUG, "Entering main indexer loop");

    while(!m_bExit) {
        // Open an admin session
        if(!m_ptrSession) {
            hr = HrOpenECSession(&m_ptrSession, L"SYSTEM", L"", m_lpConfig->GetSetting("server_socket"), 0, m_lpConfig->GetSetting("sslkey_file"),  m_lpConfig->GetSetting("sslkey_pass"));
            if(hr != hrSuccess) {
                m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to contact zarafa server %s: %08X. Will retry.", m_lpConfig->GetSetting("server_socket"), hr);
                goto next;
            }

			hr = HrOpenDefaultStore(m_ptrSession, &m_ptrAdminStore);
			if(hr != hrSuccess) {
				m_ptrSession.reset();
				m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to open system admin store: %08X", hr);
				goto next;
			}

            hr = GetServerGUID(&m_guidServer);
            if(hr != hrSuccess) {
				m_ptrSession.reset();
				m_ptrAdminStore.reset();
                goto next;
            }

            // Load the state from disk if we don't have it yet
            hr = LoadState();

            if(hr == MAPI_E_NOT_FOUND)
                m_strICSState.clear();
            else if(hr == hrSuccess) {
                m_ulIndexerState = stateRunning;
                m_lpLogger->Log(EC_LOGLEVEL_INFO, "Waiting for changes");
            } else {
                m_lpLogger->Log(EC_LOGLEVEL_ERROR, "Failed loading import state: %08X", hr);
                goto exit;
			}
        }

        if(m_ulIndexerState == stateUnknown) {
            m_lpLogger->Log(EC_LOGLEVEL_DEBUG, "Initial state, getting server state");
            hr = GetServerState(m_strICSState);
            if(hr != hrSuccess)
                goto exit;

            m_lpLogger->Log(EC_LOGLEVEL_DEBUG, "Server state after initial exports: %s", bin2hex(m_strICSState).c_str());

            // No state, start from the beginning
            m_ulIndexerState = stateBuilding;
            m_lpLogger->Log(EC_LOGLEVEL_DEBUG, "Entering build state, starting index build");
        }

        if(m_ulIndexerState == stateBuilding) {

            hr = BuildIndexes();
            if(hr != hrSuccess) {
                m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Store indexing failed");
                hr = hrSuccess;
                goto next;
            }

            m_ulIndexerState = stateRunning;
            m_lpLogger->Log(EC_LOGLEVEL_INFO, "Entering incremental state, waiting for changes");

            if(m_bExit)
                goto exit;
        }
        if(m_ulIndexerState == stateRunning) {

            hr = ProcessChanges();

            if(hr != hrSuccess) {
                m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Change processing failed: %08X", hr);
                hr = hrSuccess;
                goto next;
            }

            hr = SaveState();
            if(hr != hrSuccess) {
                m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to save server state. This may cause previous changes to be re-indexed: %08X", hr);
                hr = hrSuccess;
            }
        }

next:
        pthread_mutex_lock(&m_mutexExit);
        if(!m_bExit && !m_bFast) {
		  timespec deadline = GetDeadline(5000);
		  pthread_cond_timedwait(&m_condExit, &m_mutexExit, &deadline);
		}
        pthread_mutex_unlock(&m_mutexExit);
    }

exit:

    return hr;
}

HRESULT ECServerIndexer::ProcessChanges()
{
    HRESULT hr = hrSuccess;
    IStreamAdapter state(m_strICSState);
    UnknownPtr lpImportInterface;
    ECIndexImporter *lpImporter = NULL;
    ExchangeExportChangesPtr lpExporter;
    std::list<ECIndexImporter::ArchiveItem> lstStubTargets;

    time_t ulStartTime = time(NULL);
    time_t ulLastTime = time(NULL);
    ULONG ulSteps = 0, ulStep = 0;
    ULONG ulTotalChange = 0, ulTotalBytes = 0;
    ULONG ulCreate = 0, ulChange = 0, ulDelete = 0, ulBytes = 0;


    hr = m_ptrAdminStore->OpenProperty(PR_CONTENTS_SYNCHRONIZER, &IID_IExchangeExportChanges, 0, 0, &lpExporter);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to get system exporter: %08X", hr);
        goto exit;
    }

    hr = ECIndexImporter::Create(m_lpConfig, m_lpLogger, m_ptrAdminStore, m_lpThreadData, NULL, m_guidServer, &lpImporter);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to create stream importer: %08X", hr);
        goto exit;
    }

    hr = lpImporter->QueryInterface(IID_IUnknown, &lpImportInterface);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "QueryInterface failed for importer: %08X", hr);
        goto exit;
    }

    hr = lpExporter->Config(&state, SYNC_NORMAL, lpImportInterface, NULL, NULL, NULL, 0);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to config system exporter: %08X", hr);
        goto exit;
    }

    while(1) {
        hr = lpExporter->Synchronize(&ulSteps, &ulStep);

        if(time(NULL) > ulLastTime + 10) {
            m_lpLogger->Log(EC_LOGLEVEL_DEBUG, "Step %d of %d", ulStep, ulSteps);
        }

        if(FAILED(hr)) {
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Synchronize failed: %08X", hr);
            // Do not goto exit, we wish to save the sync state
            break;
        }

        lpImporter->GetStats(&ulCreate, &ulChange, &ulDelete, &ulBytes);

		ulTotalBytes += ulBytes;
		ulTotalChange += ulCreate + ulChange;

        if(hr == hrSuccess)
            break;

        if(m_bExit) {
            hr = MAPI_E_USER_CANCEL;
            break;
        }
    }

    // Do a second-stage index of items in archived servers
    hr = lpImporter->GetStubTargets(&lstStubTargets);
    if(hr != hrSuccess)
        goto exit;

    hr = IndexStubTargets(lstStubTargets, lpImporter);
    if(hr != hrSuccess)
        goto exit;

    pthread_mutex_lock(&m_mutexTrack);
    m_ulTrack++;
    pthread_cond_signal(&m_condTrack);
    pthread_mutex_unlock(&m_mutexTrack);

    if(lpExporter->UpdateState(&state) != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to update system sync state");
    }

    if(ulTotalChange)
       	m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_INFO, "Processed %d changes (%s) in %d seconds", ulTotalChange, str_storage(ulTotalBytes, false).c_str(), (int)(time(NULL) - ulStartTime));


exit:
    if(lpImporter)
        lpImporter->Release();

    return hr;
}

/**
 * Build indexes from scratch
 *
 * We just scan all the stores on the server and index each of them in turn. Since we
 * track state information for each folder, if the process is interrupted, the next run
 * will carry on where it left off. As an extra optimization, an index is marked 'complete'
 * when everything has been indexed, in which case it will not be re-indexed during the next
 * run.
 */
HRESULT ECServerIndexer::BuildIndexes()
{
    HRESULT hr = hrSuccess;
    ExchangeManageStorePtr lpEMS;
    MAPITablePtr lpTable;
    SRowSetPtr lpRows;
    SizedSPropTagArray(3, sptaProps) = { 3, { PR_DISPLAY_NAME_W, PR_ENTRYID, PR_EC_STORETYPE } };

    hr = m_ptrAdminStore->QueryInterface(IID_IExchangeManageStore, (void **)&lpEMS);
    if(hr != hrSuccess)
        goto exit;

    hr = lpEMS->GetMailboxTable(L"", &lpTable, 0);
    if(hr != hrSuccess)
        goto exit;

    hr = lpTable->SetColumns((LPSPropTagArray)&sptaProps, 0);
    if(hr != hrSuccess)
        goto exit;

    while(1) {
        hr = lpTable->QueryRows(20, 0, &lpRows);
        if(hr != hrSuccess)
            goto exit;

        if(lpRows.empty())
            break;

        for(unsigned int i = 0; i < lpRows.size(); i++) {
            if(m_bExit) {
                hr = MAPI_E_USER_CANCEL;
                goto exit;
            }

            m_lpLogger->Log(EC_LOGLEVEL_INFO, "Starting indexing of store %ls", WSTR(lpRows[i].lpProps[0]));

            hr = IndexStore(&lpRows[i].lpProps[1].Value.bin, lpRows[i].lpProps[2].Value.ul);
            if(hr != hrSuccess)
                goto exit;
        }
    }

exit:
    return hr;
}

/**
 * Index the entire store
 *
 * We index all the data in the store from IPM_SUBTREE and lower. This excludes associated messages and
 * folders under 'root'. Each folder is processed separately.
 */
HRESULT ECServerIndexer::IndexStore(SBinary *lpsEntryId, unsigned int ulStoreType)
{
    HRESULT hr = hrSuccess;
    MsgStorePtr lpStore;
    SPropValuePtr lpPropSubtree;
    SPropArrayPtr lpPropStore;
    SRowSetPtr lpRows;
    MAPIFolderPtr lpSubtree;
    MAPITablePtr lpHierarchy;
    SizedSPropTagArray(2, sptaFolderProps) = { 2, { PR_DISPLAY_NAME_W, PR_ENTRYID } };
    SizedSPropTagArray(2, sptaStoreProps) = { 2, { PR_MAPPING_SIGNATURE, PR_STORE_RECORD_KEY } };
    ECIndexDB *lpIndex = NULL;
    ULONG cValues = 0;
    ULONG ulObjType = 0;
    ECIndexImporter *lpImporter = NULL;

    hr = m_ptrSession->OpenMsgStore(0, lpsEntryId->cb, (LPENTRYID)lpsEntryId->lpb, NULL, 0, &lpStore);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to open store: %08X", hr);
        goto exit;
    }

    hr = lpStore->GetProps((LPSPropTagArray)&sptaStoreProps, 0, &cValues, &lpPropStore);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to get store properties: %08X", hr);
        goto exit;
    }

	m_lpLogger->Log(EC_LOGLEVEL_DEBUG, "Indexing store id %s", bin2hex(lpPropStore[1].Value.bin.cb, lpPropStore[1].Value.bin.lpb).c_str());

    hr = m_lpThreadData->lpIndexFactory->GetIndexDB(&m_guidServer, (GUID *)lpPropStore[1].Value.bin.lpb, true, false, &lpIndex);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to open index database: %08X", hr);
        goto exit;
    }

    hr = ECIndexImporter::Create(m_lpConfig, m_lpLogger, lpStore, m_lpThreadData, NULL, m_guidServer, &lpImporter);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to create stream importer: %08X", hr);
        goto exit;
    }

    if(ulStoreType == ECSTORE_TYPE_PUBLIC)
        hr = HrGetOneProp(lpStore, PR_IPM_PUBLIC_FOLDERS_ENTRYID, &lpPropSubtree);
    else
        hr = HrGetOneProp(lpStore, PR_IPM_SUBTREE_ENTRYID, &lpPropSubtree);
    if(hr != hrSuccess) {
        // If you have no IPM subtree, then we're done
        hr = hrSuccess;
        goto exit;
    }

    hr = lpStore->OpenEntry(lpPropSubtree->Value.bin.cb, (LPENTRYID)lpPropSubtree->Value.bin.lpb, &IID_IMAPIFolder, 0, &ulObjType, (IUnknown **)&lpSubtree);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to open IPM subtree: %08X", hr);
        goto exit;
    }

    hr = lpSubtree->GetHierarchyTable(CONVENIENT_DEPTH, &lpHierarchy);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to get folder list: %08X", hr);
        goto exit;
    }

    hr = lpHierarchy->SetColumns((LPSPropTagArray)&sptaFolderProps, 0);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to set column set for folder list: %08X", hr);
        goto exit;
    }

    while(1) {
        hr = lpHierarchy->QueryRows(20, 0, &lpRows);
        if(hr != hrSuccess) {
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to get folder rows: %08X", hr);
            goto exit;
        }

        if(lpRows.empty())
            break;

        for(unsigned int i=0; i < lpRows.size(); i++) {
            m_lpLogger->Log(EC_LOGLEVEL_DEBUG, "Processing folder %ls", WSTR(lpRows[i].lpProps[0]));
            hr = IndexFolder(lpStore, &lpRows[i].lpProps[1].Value.bin, WSTR(lpRows[i].lpProps[0]), lpImporter, lpIndex);
            if(hr != hrSuccess) {
                m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Indexing failed for folder: %08X. Trying to continue.", hr);
                m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Note that messages in this folder will never be found when searching. The store will need a full reindex.");
				hr = hrSuccess;
            }
        }
    }

    hr = lpIndex->SetComplete();
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_INFO, "Unable to set complete flag for index");
        goto exit;
    }

exit:
    if (lpImporter)
        lpImporter->Release();

    if(lpIndex)
         m_lpThreadData->lpIndexFactory->ReleaseIndexDB(lpIndex);

    return hr;
}

/**
 * Index the data in a folder
 *
 * This is done by requesting a synchronization stream from the server for the full folder contents, or a part
 * if a previous synchronization had stopped before the folder was done. This is done by requesting the previous
 * sync state, if any, and using that if possible.
 */

HRESULT ECServerIndexer::IndexFolder(IMsgStore *lpStore, SBinary *lpsEntryId, const WCHAR *szName, ECIndexImporter *lpImporter, ECIndexDB *lpIndex)
{
    HRESULT hr = hrSuccess;
    HRESULT hrSaveState = hrSuccess;
    SPropArrayPtr lpProps;
    MAPIFolderPtr lpFolder;
    std::string strFolderId((char *)lpsEntryId->lpb, lpsEntryId->cb);
    std::string strFolderState;
    std::string strStubTargetState;
    ULONG ulSteps = 0, ulStep = 0;
    ULONG cValues = 0;
    ULONG ulObjType = 0;
    IStreamAdapter state(strFolderState);
    ExchangeExportChangesPtr lpExporter;
    UnknownPtr lpImportInterface;

	time_t ulLastStatsTime = time(NULL);
	ULONG ulStartTime = ulLastStatsTime;
	ULONG ulBlockChange = 0;
	ULONG ulBlockBytes = 0;
	ULONG ulBytes = 0;
	ULONG ulTotalChange = 0;
	ULONG ulChange = 0, ulCreate = 0, ulDelete = 0;
	unsigned long long ulTotalBytes = 0;

    std::list<ECIndexImporter::ArchiveItem> lstStubTargets;

    SizedSPropTagArray(3, sptaProps) = {3, { PR_DISPLAY_NAME_W, PR_MAPPING_SIGNATURE, PR_STORE_RECORD_KEY } };

    hr = lpImporter->QueryInterface(IID_IUnknown, &lpImportInterface);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to get importer interface: %08X", hr);
        goto exit;
    }

    hr = lpStore->GetProps((LPSPropTagArray)&sptaProps, 0, &cValues, &lpProps);
    if(FAILED(hr))
        goto exit;

    lpIndex->GetSyncState(strFolderId, strFolderState, strStubTargetState); // ignore error and use empty state if this fails

    if (!strStubTargetState.empty()) {
        hr = lpImporter->LoadStubTargetState(strStubTargetState, &lstStubTargets);
        if (hr != hrSuccess) {
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to load saved stub target state: %08X", hr);
            goto exit;
        } else {
            if (!lstStubTargets.empty())
                m_lpLogger->Log(EC_LOGLEVEL_DEBUG, "Loaded %u stub targets from saved state", (unsigned int)lstStubTargets.size());
        }
    }

    hr = lpStore->OpenEntry(lpsEntryId->cb, (LPENTRYID)lpsEntryId->lpb, &IID_IMAPIFolder, 0, &ulObjType, (IUnknown **)&lpFolder);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to open folder %ls in store %ls, for indexing: %08X", szName, WSTR(lpProps[0]), hr);
        goto exit;
    }

    // Start a synchronization loop
    hr = lpFolder->OpenProperty(PR_CONTENTS_SYNCHRONIZER, &IID_IExchangeExportChanges, 0, 0, (IUnknown **)&lpExporter);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to open exporter for folder %ls: %08X", szName, hr);
        goto exit;
    }

    hr = lpExporter->Config(&state, SYNC_NORMAL, lpImportInterface, NULL, NULL, NULL, 0);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable configure exporter: %08X", hr);
        goto exit;
    }

    while(1) {
        hr = lpExporter->Synchronize(&ulSteps, &ulStep);
        if(FAILED(hr)) {
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Synchronize failed: %08X", hr);
            // Do not goto exit, we wish to save the sync state
            break;
        }

        lpImporter->GetStats(&ulCreate, &ulChange, &ulDelete, &ulBytes);

		ulTotalBytes += ulBytes;
		ulTotalChange += ulCreate + ulChange;

		ulBlockBytes += ulBytes;
		ulBlockChange += ulCreate + ulChange;

        if(hr == hrSuccess || m_bExit)
            break;

		if (ulLastStatsTime < time(NULL) - 10) {
			unsigned int secs = time(NULL) - ulLastStatsTime;
			m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_INFO, "%.1f%% (%d of %d) of folder '%ls' processed: %s in %d messages (%s/sec, %.1f messages/sec)", ((float)ulStep*100)/ulSteps,ulStep,ulSteps, szName, str_storage(ulBlockBytes, false).c_str(), ulBlockChange, str_storage(ulBlockBytes/secs, false).c_str(), (float)ulBlockChange/secs);
			ulLastStatsTime = time(NULL);
			ulBlockBytes = ulBlockChange = 0;
		}

    }

    // Get the list of stub targets, either for processing or for saving in combination
    // with the sync state.
    lpImporter->GetStubTargets(&lstStubTargets);  // This can't fail

    // Don't attempt to process the stubs if exit was requested or a previous error has occurred.
    if (!FAILED(hr)) {
        if (!m_bExit) {
            // Do a second-stage index of items in archived servers
            hr = IndexStubTargets(lstStubTargets, lpImporter);
            // Do not goto exit, we wish to save the sync state
        } else
            hr = MAPI_E_USER_CANCEL;
    }

    // Failure to save the state is non-fatal. It can only cause a rescan of the folder
    // in case the index of the store is incomplete and the server is restarted.
    if (FAILED(hr)) {
        hrSaveState = lpImporter->SaveStubTargetState(lstStubTargets, strStubTargetState);
        if (hrSaveState != hrSuccess) {
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to get stub target state: %08X", hrSaveState);
            goto exit;
        }
		m_lpLogger->Log(EC_LOGLEVEL_DEBUG, "Got %u bytes of stub target state", (unsigned int)strStubTargetState.size());
    } else {
        // Clear the stub target state if we completed successfully
        strStubTargetState.clear();
    }

    hrSaveState = lpExporter->UpdateState(&state);
    if (hrSaveState != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to update sync state: %08X", hrSaveState);
        goto exit;
    }

    hrSaveState = lpIndex->SetSyncState(strFolderId, strFolderState, strStubTargetState);
    if (hrSaveState != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to save sync state: %08X", hrSaveState);
        goto exit;
    }

    if(ulTotalChange || m_lpLogger->Log(EC_LOGLEVEL_DEBUG))
    	m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_INFO, "Processed folder '%ls' with %d changes (%s) in %d seconds", szName, ulTotalChange, str_storage(ulTotalBytes, false).c_str(), (int)(time(NULL) - ulStartTime));

exit:
    return hr;
}

/**
 * Index stub targets
 *
 * This function retrieves data from archiveservers and indexes the data there as if the content
 * were in the original stub documents.
 *
 * @param[in] lstStubTargets A List of all stub targets to process
 */
HRESULT ECServerIndexer::IndexStubTargets(const std::list<ECIndexImporter::ArchiveItem> &lstStubTargets, ECIndexImporter *lpImporter)
{
    HRESULT hr = hrSuccess;
    std::map<std::string, std::list<std::string> > mapArchiveServers;
    std::map<std::string, std::list<std::string> >::iterator server;
    std::list<ECIndexImporter::ArchiveItem>::const_iterator i;
    std::string strServerName;

    m_lpLogger->Log(EC_LOGLEVEL_DEBUG, "Indexing %lu stub targets", lstStubTargets.size());

    // Sort the items into groups per archive server
    for(i = lstStubTargets.begin(); i != lstStubTargets.end(); i++) {
        hr = HrExtractServerName((LPENTRYID)i->strStoreId.data(), i->strStoreId.size(), strServerName);
        if(hr != hrSuccess) {
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to extract server name from store entryid: %08X", hr);
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Skipping stub target.");
            hr = hrSuccess;
            continue;
        }

        mapArchiveServers[strServerName].push_back(i->strEntryId);
    }

    for(server = mapArchiveServers.begin() ; server != mapArchiveServers.end(); server++) {
        hr = IndexStubTargetsServer(server->first, server->second, lpImporter);
        if(hr != hrSuccess) {
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to index stubs on server '%s': %08X", server->first.c_str(), hr);
            goto exit;
        }
    }

exit:
    return hr;
}

/**
 * Index stub targets on a specific server
 *
 * This means we have to connect to the server, and the request all the data on that server
 * for the messages in question. We do this in blocks of N messages so that the requests do not become
 * too large.
 *
 * @param[in] strServerName Pseudo-name of server to connect with
 * @param[in] mapArchiveItems Map containing document id -> entryid
 */

HRESULT ECServerIndexer::IndexStubTargetsServer(const std::string &strServer, const std::list<std::string> &mapItems, ECIndexImporter *lpImporter)
{
    HRESULT hr = hrSuccess;
    MsgStorePtr lpRemoteStore;
    ECExportChangesPtr lpExporter;
	time_t ulLastStatsTime = time(NULL);
	ULONG ulStartTime = ulLastStatsTime;
    ULONG ulSteps = 0, ulStep = 0;
    ULONG ulTotalBytes = 0;
	ULONG ulBlockChange = 0;
	ULONG ulBlockBytes = 0;
	ULONG ulBytes = 0;
	ULONG ulTotalChange = 0;
	ULONG ulChange = 0, ulCreate = 0, ulDelete = 0;
	EntryListPtr lpEntryList;
	unsigned int n = 0;
    UnknownPtr lpImportInterface;
    set<SBinary> setEntries;

    m_lpLogger->Log(EC_LOGLEVEL_INFO, "Indexing %lu stubs on server '%s'", mapItems.size(), strServer.c_str());

    hr = lpImporter->QueryInterface(IID_IUnknown, &lpImportInterface);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "QueryInterface failed for importer: %08X", hr);
        goto exit;
    }

    hr = HrGetRemoteAdminStore(m_ptrSession, m_ptrAdminStore, (TCHAR *)strServer.c_str(), 0, &lpRemoteStore);
    if(hr != hrSuccess) {
        if(hr == MAPI_E_NOT_FOUND) {
            // The idea of ignoring this is that when people take servers out of commission completely, they are apparently no longer
            // interested in the data
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to contact server '%s': Server not found. Skipping stubs on this server.", strServer.c_str());
            hr = hrSuccess;
            goto exit;
        } else {
            // Most other errors (network error, logon failed) are unintentional, therefore we should give the sysadmin the chance to fix the problem before continuing.
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to open remote admin store on server '%s': %08X. Make sure your SSL authentication is properly configured for access from this host to the remote host.", strServer.c_str(), hr);
            goto exit;
        }
    }

    hr = lpRemoteStore->OpenProperty(PR_CONTENTS_SYNCHRONIZER, &IID_IECExportChanges, 0, 0, &lpExporter);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to open exporter on remote server '%s': %08X", strServer.c_str(), hr);
        goto exit;
    }

    hr = MAPIAllocateBuffer(sizeof(ENTRYLIST), (void **)&lpEntryList);
    if(hr != hrSuccess)
        goto exit;

    hr = MAPIAllocateMore(sizeof(SBinary) * mapItems.size(), lpEntryList, (void **)&lpEntryList->lpbin);
    if(hr != hrSuccess)
        goto exit;

    // Cheap copy data from mapItems into lpEntryList
    for(std::list<std::string>::const_iterator i = mapItems.begin(); i != mapItems.end(); i++) {
        SBinary bin = {(unsigned int)i->size(), (BYTE *)i->data()};

        // We avoid duplicate entries in lpEntryList as the rowengine
        // doesn't like duplicates.
        // As a side effect only one out of multiple messages referencing
        // the same archive message will have the content of the archive
        // added to it's index data.
        if (setEntries.insert(bin).second == true) {
            // true indicates a new item was added to the set.
            lpEntryList->lpbin[n] = bin;
            n++;
        } else
            m_lpLogger->Log(EC_LOGLEVEL_INFO, "Removing duplicate entry from stub target list");
    }
    lpEntryList->cValues = n;

    hr = lpExporter->ConfigSelective(PR_ENTRYID, lpEntryList, NULL, 0, lpImportInterface, NULL, NULL, 0);
    if(hr != hrSuccess) {
        if(hr == MAPI_E_NO_SUPPORT)
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Remote server '%s' does not support streaming destub. Please upgrade the remote server to Zarafa version 7.1 or later", strServer.c_str());
        else
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to configure stub exporter on server '%s': %08X", strServer.c_str(), hr);
        goto exit;
    }

    while(1) {
        hr = lpExporter->Synchronize(&ulSteps, &ulStep);
        if(FAILED(hr)) {
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Synchronize failed during destub: %08X", hr);
            // Do not goto exit, we wish to save the sync state
            break;
        }

        lpImporter->GetStats(&ulCreate, &ulChange, &ulDelete, &ulBytes);

		ulTotalBytes += ulBytes;
		ulTotalChange += ulCreate + ulChange;

		ulBlockBytes += ulBytes;
		ulBlockChange += ulCreate + ulChange;

        if(hr == hrSuccess)
            break;

        if(m_bExit) {
            hr = MAPI_E_USER_CANCEL;
            break;
        }

		if (ulLastStatsTime < time(NULL) - 10) {
			unsigned int secs = time(NULL) - ulLastStatsTime;
			m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_INFO, "%.1f%% (%d of %d) of archived stubs processed: %s in %d messages (%s/sec, %.1f messages/sec)", ((float)ulStep*100)/ulSteps,ulStep,ulSteps, str_storage(ulBlockBytes, false).c_str(), ulBlockChange, str_storage(ulBlockBytes/secs, false).c_str(), (float)ulBlockChange/secs);
			ulLastStatsTime = time(NULL);
			ulBlockBytes = ulBlockChange = 0;
		}

    }

    if(ulTotalChange || m_lpLogger->Log(EC_LOGLEVEL_DEBUG))
    	m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_INFO, "Processed archive data with %d changes (%s) in %d seconds", ulTotalChange, str_storage(ulTotalBytes, false).c_str(), (int)(time(NULL) - ulStartTime));

exit:
    return hr;
}


/**
 * Gets the current server-wide ICS state from the server
 *
 * This is used to determine from what state we need to replay server-wide
 * ICS events once we're done exporting data from all the stores.
 */
HRESULT ECServerIndexer::GetServerState(std::string &strServerState)
{
    HRESULT hr = hrSuccess;
    std::string strState;
    IStreamAdapter state(strState);
    ExchangeExportChangesPtr lpExporter;
    ULONG ulSteps = 0, ulStep = 0;

    hr = m_ptrAdminStore->OpenProperty(PR_CONTENTS_SYNCHRONIZER, &IID_IExchangeExportChanges, 0, 0, &lpExporter);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to open systemwide exporter: %08X", hr);
        goto exit;
    }

    hr = lpExporter->Config(&state, SYNC_NORMAL | SYNC_CATCHUP, NULL, NULL, NULL, NULL, 0);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to config systemwide exporter: %08X", hr);
        goto exit;
    }

    while(1) {
        hr = lpExporter->Synchronize(&ulSteps, &ulStep);

        if(FAILED(hr)) {
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Synchronize failed on systemwide exporter: %08X", hr);
            goto exit;
        }

        if(hr == hrSuccess)
            break;
    }

    lpExporter->UpdateState(&state);

    strServerState = strState;

exit:
    return hr;
}

/**
 * Load server-wide state from disk
 */
HRESULT ECServerIndexer::LoadState()
{
    HRESULT hr = hrSuccess;
    std::string strFn = (std::string)m_lpConfig->GetSetting("index_path") + PATH_SEPARATOR + "state-" + bin2hex(sizeof(GUID), (unsigned char *)&m_guidServer);
    int c = 0;

    FILE *fp = fopen(strFn.c_str(), "rb");

    if(!fp) {
        if(errno != ENOENT) {
            m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to open '%s' for reading: %s", strFn.c_str(), strerror(errno));
            hr = MAPI_E_DISK_ERROR;
        } else {
            hr = MAPI_E_NOT_FOUND;
        }
        goto exit;
    }

    m_strICSState.clear();

    while((c = fgetc(fp)) != EOF)
        m_strICSState.append(1, c);

exit:
    if(fp)
        fclose(fp);

    return hr;
}

/**
 * Save server-wide state to disk
 */
HRESULT ECServerIndexer::SaveState()
{
    HRESULT hr = hrSuccess;
    std::string strFn = (std::string)m_lpConfig->GetSetting("index_path") + PATH_SEPARATOR + "state-" + bin2hex(sizeof(GUID), (unsigned char *)&m_guidServer);

    FILE *fp = fopen(strFn.c_str(), "wb");

    if(!fp) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to open '%s' for writing: %s", strFn.c_str(), strerror(errno));
        hr = MAPI_E_DISK_ERROR;
        goto exit;
    }

    if(fwrite(m_strICSState.data(), 1, m_strICSState.size(), fp) != m_strICSState.size()) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to write state to '%s': %s", strFn.c_str(), strerror(errno));
        hr = MAPI_E_DISK_ERROR;
        goto exit;
    }

exit:
    if(fp)
        fclose(fp);

    return hr;
}

/**
 * Get GUID for the server we're connected to in
 */
HRESULT ECServerIndexer::GetServerGUID(GUID *lpGuid)
{
    HRESULT hr = hrSuccess;
    SPropValuePtr lpProp;

    hr = HrGetOneProp(m_ptrAdminStore, PR_MAPPING_SIGNATURE, &lpProp);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to get server guid: %08X", hr);
        goto exit;
    }

    *lpGuid = *(GUID*)lpProp->Value.bin.lpb;

exit:
    return hr;
}

/**
 * Run synchronization at least once.
 *
 */
HRESULT ECServerIndexer::RunSynchronization()
{
    HRESULT hr = hrSuccess;

    unsigned int ulTrack = m_ulTrack;

    pthread_mutex_lock(&m_mutexExit);
    m_bFast = true;
    pthread_cond_signal(&m_condExit);
    pthread_mutex_unlock(&m_mutexExit);

    pthread_mutex_lock(&m_mutexTrack);
    while(m_ulTrack < ulTrack + 2) 			// Wait for two whole sync loops, since 1 may just have missed an update
        pthread_cond_wait(&m_condTrack, &m_mutexTrack);
    pthread_mutex_unlock(&m_mutexTrack);

    // No need to signal now
    m_bFast = false;

    return hr;
}

/**
 * Removes the current index of a given user on a server, and starts the indexer
 *
 * @param strStoreGuid
 *
 * @return
 */
HRESULT ECServerIndexer::ReindexStore(const std::string &strServerGuid, const std::string &strStoreGuid)
{
	HRESULT hr = hrSuccess;

	pthread_mutex_lock(&m_mutexRebuild);

	// MAPI_E_NOT_ME when serverguid != m_guidServer ?

	if (m_ulIndexerState < stateRunning) {
		m_lpLogger->Log(EC_LOGLEVEL_ERROR, "Reindex of store %s-%s impossible, indexer in wrong state", strServerGuid.c_str(), strStoreGuid.c_str());
		hr = MAPI_E_BUSY;
		goto exit;
	}

    hr = m_lpThreadData->lpIndexFactory->RemoveIndexDB(strServerGuid, strStoreGuid);
    if(hr != hrSuccess) {
        m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to remove index database: %08X", hr);
        goto exit;
    }

	m_listRebuildStores.push_back(make_pair<std::string, std::string>(strServerGuid, strStoreGuid));
    pthread_cond_signal(&m_condRebuild);

exit:
	pthread_mutex_unlock(&m_mutexRebuild);

	return hr;
}

/**
 * Rebuilding thread entry point back into class space.
 *
 * @param lpParam ECServerIndexer instance
 *
 * @return NULL
 */
void *ECServerIndexer::ThreadRebuildEntry(void *lpParam)
{
    ECServerIndexer *lpIndexer = (ECServerIndexer *)lpParam;
	lpIndexer->ThreadRebuild();
	return NULL;
}

/**
 * Rebuilding thread. Waits for condition to exit or rebuild a store.
 *
 * @return hrSuccess
 */
HRESULT ECServerIndexer::ThreadRebuild()
{
	HRESULT hr = hrSuccess;
    ExchangeManageStorePtr ptrEMS;
    MAPITablePtr ptrTable;
    SRowSetPtr ptrRows;
    SizedSPropTagArray(3, sptaProps) = { 3, { PR_DISPLAY_NAME_W, PR_ENTRYID, PR_EC_STORETYPE } };
	std::string strServerGuid, strStoreGuid;
	SPropValue propServerGuid, propStoreGuid;

	m_lpLogger->Log(EC_LOGLEVEL_INFO, "Starting rebuild thread");

	pthread_mutex_lock(&m_mutexRebuild);
	while (!m_bExit) {
		if (m_listRebuildStores.empty())
			pthread_cond_wait(&m_condRebuild, &m_mutexRebuild);

		if (m_bExit)
			break;

		if (m_listRebuildStores.empty())
			continue;

		strServerGuid = hex2bin(m_listRebuildStores.front().first);
		strStoreGuid = hex2bin(m_listRebuildStores.front().second);
		m_listRebuildStores.pop_front();

		pthread_mutex_unlock(&m_mutexRebuild);

		// lookup store

		hr = m_ptrAdminStore->QueryInterface(IID_IExchangeManageStore, (void **)&ptrEMS);
		if(hr != hrSuccess)
			goto next;

		hr = ptrEMS->GetMailboxTable(NULL, &ptrTable, 0);
		if(hr != hrSuccess)
			goto next;

		hr = ptrTable->SetColumns((LPSPropTagArray)&sptaProps, 0);
		if(hr != hrSuccess)
			goto next;

		propServerGuid.ulPropTag = PR_MAPPING_SIGNATURE;
		propServerGuid.Value.bin.cb = strServerGuid.length();
		propServerGuid.Value.bin.lpb = (BYTE*)strServerGuid.data();
		propStoreGuid.ulPropTag = PR_STORE_RECORD_KEY;
		propStoreGuid.Value.bin.cb = strStoreGuid.length();
		propStoreGuid.Value.bin.lpb = (BYTE*)strStoreGuid.data();

		hr = ECAndRestriction(
				ECPropertyRestriction(RELOP_EQ, PR_MAPPING_SIGNATURE, &propServerGuid, ECRestriction::Shallow) +
				ECPropertyRestriction(RELOP_EQ, PR_STORE_RECORD_KEY, &propStoreGuid, ECRestriction::Shallow)
			 ).FindRowIn(ptrTable, BOOKMARK_BEGINNING, 0);
		if(hr != hrSuccess)
			goto next;

		hr = ptrTable->QueryRows(1, 0, &ptrRows);
		if(hr != hrSuccess)
			goto next;

		// start building index

		hr = IndexStore(&ptrRows[0].lpProps[1].Value.bin, ptrRows[0].lpProps[2].Value.ul);

next:
		if (hr != hrSuccess)
			m_lpLogger->Log(EC_LOGLEVEL_FATAL, "Unable to reindex store, 0x%08X", hr);
		pthread_mutex_lock(&m_mutexRebuild);
	}
	pthread_mutex_unlock(&m_mutexRebuild);
	m_lpLogger->Log(EC_LOGLEVEL_INFO, "Stopping rebuild thread");

	return hrSuccess;
}
