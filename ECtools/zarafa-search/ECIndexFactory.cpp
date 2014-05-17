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

#include <mapix.h>
#include "stringutil.h"
#include "Util.h"

#include "ECIndexFactory.h"
#include "ECIndexDB.h"

/**
 * The indexfactory manages concurrent access to the same index database.
 *
 * Since the API only allows us to open one index at once, but does allow
 * concurrent access to that object, we keep a list of 'open' indexes here. This
 * allows searching in */

ECIndexFactory::ECIndexFactory(ECConfig *lpConfig, ECLogger *lpLogger)
{
    m_lpConfig = lpConfig;
    m_lpLogger = lpLogger;
    
    pthread_mutex_init(&m_mutexIndexes, NULL);
}

ECIndexFactory::~ECIndexFactory()
{
    std::map<std::string, std::pair<unsigned int, ECIndexDB *> >::iterator i;

    for(i = m_mapIndexes.begin(); i != m_mapIndexes.end(); i++) {
        delete i->second.second;
    }
}

HRESULT ECIndexFactory::GetIndexDB(GUID *lpServer, GUID *lpStore, bool bCreate, bool bComplete, ECIndexDB **lppIndexDB)
{
    HRESULT hr = hrSuccess;
    std::map<std::string, std::pair<unsigned int, ECIndexDB *> >::iterator i;
    
    std::string strStore = GetStoreId(lpServer, lpStore);

    pthread_mutex_lock(&m_mutexIndexes);
    i = m_mapIndexes.find(strStore);
    
    if (i != m_mapIndexes.end()) {
        *lppIndexDB = i->second.second;
        i->second.first++;
    } else {
        hr = ECIndexDB::Create(strStore, m_lpConfig, m_lpLogger, bCreate, bComplete, lppIndexDB);
        if(hr != hrSuccess)
            goto exit;
            
        m_mapIndexes.insert(std::make_pair(strStore, std::make_pair(1, *lppIndexDB)));
    }
    
exit:
    pthread_mutex_unlock(&m_mutexIndexes);
    
    return hr;
}

HRESULT ECIndexFactory::ReleaseIndexDB(ECIndexDB *lpIndexDB)
{
    HRESULT hr = hrSuccess;
    std::map<std::string, std::pair<unsigned int, ECIndexDB *> >::iterator i;
    
    pthread_mutex_lock(&m_mutexIndexes);

    for(i = m_mapIndexes.begin(); i != m_mapIndexes.end(); i++) {
        if (i->second.second == lpIndexDB) {
            i->second.first--;
            
            if(i->second.first == 0) {
                delete i->second.second;
                m_mapIndexes.erase(i);
            }
            
            break;
        }
    }

    pthread_mutex_unlock(&m_mutexIndexes);
    
    return hr;
}

HRESULT ECIndexFactory::RemoveIndexDB(const GUID &guidServer, const GUID &guidStore) 
{
	std::string strServerGuid = bin2hex(sizeof(GUID), (unsigned char *)&guidServer);
	std::string strStoreGuid = bin2hex(sizeof(GUID), (unsigned char *)&guidStore);

	return RemoveIndexDB(strServerGuid, strStoreGuid);
}

HRESULT ECIndexFactory::RemoveIndexDB(const std::string &strServerGuid, const std::string &strStoreGuid)
{
	HRESULT hr = hrSuccess;
    std::map<std::string, std::pair<unsigned int, ECIndexDB *> >::iterator i;
	std::string strStore = strServerGuid + "-" + strStoreGuid;
	ECIndexDB *lpIndexDB = NULL;

    pthread_mutex_lock(&m_mutexIndexes);
    i = m_mapIndexes.find(strStore);
    
    if (i != m_mapIndexes.end()) {
		// index is being used
		hr = MAPI_E_BUSY;
    } else {
        hr = ECIndexDB::Create(strStore, m_lpConfig, m_lpLogger, false, false, &lpIndexDB);
		if (hr == MAPI_E_NOT_FOUND) {
			hr = hrSuccess;
			goto exit;
		}
        if(hr != hrSuccess)
            goto exit;

		hr = lpIndexDB->Remove();
    }

exit:
    pthread_mutex_unlock(&m_mutexIndexes);
	delete lpIndexDB;
    return hr;
}

std::string ECIndexFactory::GetStoreId(GUID *lpServer, GUID *lpStore)
{
    return bin2hex(sizeof(GUID), (unsigned char *)lpServer) + "-" + bin2hex(sizeof(GUID), (unsigned char *)lpStore);
}

/** 
 * Parses a filename to return the server and store guids
 * 
 * @param[in] strFilename filename of the index without path
 * @param[out] lpServer server guid
 * @param[out] lpStore store guid
 * 
 * @return MAPI Error code
 */
HRESULT ECIndexFactory::GetStoreIdFromFilename(const std::string &strFilename, GUID *lpServer, GUID *lpStore)
{
	HRESULT hr = MAPI_E_INVALID_PARAMETER;
	GUID guidServer, guidStore;
	
	// serverguid - storeguid .kct
	if (strFilename.length() != 32 + 1 + 32 + 4)
		goto exit;

	if (Util::hex2bin(strFilename.c_str() + 0, 32, (LPBYTE)&guidServer) != hrSuccess)
		goto exit;
	if (Util::hex2bin(strFilename.c_str() + 32+1, 32, (LPBYTE)&guidStore) != hrSuccess)
		goto exit;

	hr = hrSuccess;
	*lpServer = guidServer;
	*lpStore = guidStore;

exit:
	return hr;
}
