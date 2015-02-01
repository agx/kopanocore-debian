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

#include <platform.h>

#include <mapi.h>
#include <mapix.h>
#include <mapiutil.h>
#include <mapidefs.h>
#include <mapiguid.h>

#include <edkguid.h>
#include <edkmdb.h>

#include <ECGuid.h>
#include <ECSerializer.h>
#include <ECTags.h>
#include <IECSingleInstance.h>
#include <stringutil.h>
#include <UnixUtil.h>
#include <ZarafaCode.h>

#include "ECIndexerUtil.h"
#include "ECIndexImporter.h"
#include "ECIndexImporterAttachments.h"
#include "ECIndexDB.h"

#include "stringutil.h"
#include "charset/convert.h"

#define STREAM_BUFFER   ( 64*1024 )

using namespace std;

ECIndexImporterAttachment::ECIndexImporterAttachment(ECThreadData *lpThreadData, ECIndexImporter *lpIndexer)
{
	m_lpThreadData = lpThreadData;
	m_lpLogger = m_lpThreadData->lpLogger;
	m_lpIndexer = lpIndexer;
	m_lpCache = NULL;
	m_ulCache = 0;

	m_strCommand = m_lpThreadData->m_strCommand;
}

ECIndexImporterAttachment::~ECIndexImporterAttachment()
{
	if (m_lpCache)
		MAPIFreeBuffer(m_lpCache);
}

HRESULT ECIndexImporterAttachment::Create(ECThreadData *lpThreadData, ECIndexImporter *lpIndexer, ECIndexImporterAttachment **lppIndexerAttach)
{
	HRESULT hr = hrSuccess;
	ECIndexImporterAttachment *lpIndexerAttach = NULL;

	try {
		lpIndexerAttach = new ECIndexImporterAttachment(lpThreadData, lpIndexer);
	}
	catch (...) {
		lpIndexerAttach = NULL;
	}
	if (!lpIndexerAttach) {
		hr = MAPI_E_NOT_ENOUGH_MEMORY;
		goto exit;
	}

	lpIndexerAttach->AddRef();

	hr = MAPIAllocateBuffer(STREAM_BUFFER, (LPVOID *)&lpIndexerAttach->m_lpCache);
	if (hr != hrSuccess)
		goto exit;

	lpIndexerAttach->m_ulCache = STREAM_BUFFER;

	if (lppIndexerAttach)
		*lppIndexerAttach = lpIndexerAttach;

exit:
	if ((hr != hrSuccess) && lpIndexerAttach)
		lpIndexerAttach->Release();

	return hr;
}

/**
 * Copy some data to the parser command
 *
 * This function feeds the parser with some new data to work with
 */
HRESULT ECIndexImporterAttachment::CopyBlockToParser(IStream *lpStream, int ulFpWrite, ULONG *lpulSize)
{
	HRESULT hr = hrSuccess;
	ULONG ulStreamData = 0;
	ssize_t ulSize = 0;

	hr = lpStream->Read(m_lpCache, m_ulCache, &ulStreamData);
	if (hr != hrSuccess) {
		hr = hrSuccess;
		goto exit;
	}

	if (ulStreamData == 0)
		goto exit;

	ulSize = write(ulFpWrite, m_lpCache, ulStreamData);
	if (ulSize < 0) {
		// reset return substraction
		ulSize = 0;

		/*
		 * This occurs when no parser is available for this attachment
		 */
		if (errno == EPIPE) {
			// end of parser command
			hr = hrSuccess;
			goto exit;
		}

		/*
		 * When EINTR is signalled there doesn't have to be a problem, it usually means that
		 * that total amount of data written is smaller then the amount of data provided.
		 */
		if (errno == EINTR) {
			hr = hrSuccess;
			goto exit;
		}

		/* Fatal error */
		hr = MAPI_E_CALL_FAILED;
		goto exit;
	} else if (ulSize == 0) {
		hr = MAPI_E_CALL_FAILED;
		goto exit;
	}

exit:
	// In all cases do we return the number of bytes written to the attachment parser
	if (lpulSize)
		*lpulSize = ulSize;

	return hr;
}

/**
 * Read some data from the parser
 *
 * This function reads the plaintext data from the parser and converts it into wchars so
 * it can be indexed.
 */
HRESULT ECIndexImporterAttachment::CopyBlockFromParser(int ulFpRead, std::wstring *strInput)
{
	HRESULT hr = hrSuccess;
	ssize_t ulSize = 0;

	memset(m_lpCache, 0, m_ulCache);

	ulSize = read(ulFpRead, m_lpCache, m_ulCache);
	if (ulSize < 0) {
		/*
		 * EINTR should be retried. read() was signalled before it could read from the pipe.
		 */
		if (errno != EINTR)
			goto exit;

		hr = MAPI_E_CALL_FAILED;
		goto exit;
	} else if (ulSize > 0) {
		/* Data has been read */
		
		// Force conversion to keep trying by adding FORCE,IGNORE. This means we will ignore characters that
		// cannot be converted to the current charset (which is unlikely), and also ignore invalid input
		// characters.
		std::string strToCode = CHARSET_TCHAR;
		strToCode.append("//FORCE,IGNORE");
		strInput->append(convert_to<std::wstring>(strToCode.c_str(), (char *)m_lpCache, ulSize, "UTF-8"));
	} else {
		/* No data has been read, probably end-of-file */
		hr = MAPI_E_CANCEL;
	}

exit:
	return hr;
}

/**
 * Simultaneously write to the parser command and read the output
 *
 * The reason we have to interleave reading and writing is that the command may block if we write to it without reading. So
 * this function uses select() to read as much data from the command as possible after, and only writes when there is room to
 * write more data. Note that this can in theory still deadlock, since a single write to the parser command may produce so
 * much output that the parser itself blocks for the data to be read. However, since we are using a fairly small blocksize and
 * the output is normally smaller than the input, this doesn't happen in practice.
 */
HRESULT ECIndexImporterAttachment::CopyStreamToParser(IStream *lpStream, int ulFpWrite, int ulFpRead, std::wstring *strInput)
{
	HRESULT hr = hrSuccess;
	HRESULT hrWrite = hrSuccess, hrRead = hrSuccess;
	STATSTG sStat;
	LARGE_INTEGER llSeek = { {0, 0} };
	ULONG ulTotalWriteData;
	ULONG ulWriteData;
	fd_set rset, wset;
	int res;
	int fd = max(ulFpWrite, ulFpRead);
	struct timeval timeout;


	hr = lpStream->Stat(&sStat, STATFLAG_DEFAULT);
	if (hr != hrSuccess)
		goto exit;

	ulTotalWriteData = sStat.cbSize.LowPart;
	if (sStat.cbSize.HighPart || (ulTotalWriteData > m_lpThreadData->m_ulAttachMaxSize)) {
		hr = MAPI_E_TOO_BIG;
		goto exit;
	}

	while (true)
	{
		FD_ZERO(&rset);
		FD_SET(ulFpRead, &rset);
		FD_ZERO(&wset);
		if (ulTotalWriteData) {
			FD_SET(ulFpWrite, &wset);
			fd = max(ulFpWrite, ulFpRead);
		} else {
			fd = ulFpRead;
		}

		timeout.tv_sec = 10;
		timeout.tv_usec = 0;

		res = select(fd+1, &rset, &wset, NULL, &timeout);
		if (res < 0) {
			if (errno == EINTR) {
				// normal end of script
				hr = hrSuccess;
				goto exit;
			} else {
				// other error
				hr = MAPI_E_CALL_FAILED;
				goto exit;
			}
		} else if (res == 0) {
			// script failed to respond on time
			hr = MAPI_E_TIMEOUT;
			goto exit;
		}

		/*
		 * We first read from the parser to clear the pipe
		 * buffer. This should make our write also succeed without
		 * blocking.
		 */
		if (FD_ISSET(ulFpRead, &rset)) {
			// read results from parser
			hrRead = CopyBlockFromParser(ulFpRead, strInput);
			if (hrRead == MAPI_E_CANCEL) {
				// end of parser script
				hr = hrSuccess;
				goto exit;
			}
		}
		if (FD_ISSET(ulFpWrite, &wset)) {
			// write attachment to parser
			hrWrite = CopyBlockToParser(lpStream, ulFpWrite, &ulWriteData);
			if (hrWrite == hrSuccess) {
				ulTotalWriteData -= ulWriteData;
				if (ulTotalWriteData == 0) {
					// close write pipe, so attachment script gets a pipe signal (required for zmktemp script)
					close(ulFpWrite);
					ulFpWrite = 0;
				}
			}
		}
		if (hrWrite != hrSuccess)
			hr = hrWrite;
		if (hrRead != hrSuccess)
			hr = hrRead;
		if (hr != hrSuccess)
			goto exit;

		/*
		 * Always make sure we seek to the correct position to guarentee the entire attachment
		 * is send to the parser, since CopyBlockToParser() may write less than requested.
		 */
		if (ulTotalWriteData) {
			llSeek.QuadPart = sStat.cbSize.QuadPart - ulTotalWriteData;
			hr = lpStream->Seek(llSeek, STREAM_SEEK_SET, NULL);
			if (hr != hrSuccess)
				goto exit;
		}
	}

exit:
	// since we may or may not close the fd, we make sure we always do it to avoid ugly situations
	if (ulFpWrite)
		close(ulFpWrite);

	if (ulFpRead)
		close(ulFpRead);

	return hr;
}

HRESULT ECIndexImporterAttachment::ParseAttachmentCommand(tstring &strFilename, std::string &strCommand, IStream *lpStream, std::wstring *lpstrParsed)
{
	HRESULT hr = hrSuccess;
	pid_t ulCommandPid = 0;
	int ulCommandRetval = 0;
	int ulFpWrite = -1;
	int ulFpRead = -1;

	sized_popen_rlimit_array(2, sSystemLimits) = {
		2, {
			{ RLIMIT_AS, { m_lpThreadData->m_ulParserMaxMemory, m_lpThreadData->m_ulParserMaxMemory } },
			{ RLIMIT_CPU, { m_lpThreadData->m_ulParserMaxCpuTime, m_lpThreadData->m_ulParserMaxCpuTime } },
		},
	};


	m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_DEBUG, "Starting attachment parser command: \"%s\" for file '%ls'", strCommand.c_str(), strFilename.c_str());

	ulCommandPid = unix_popen_rw(m_lpThreadData->lpLogger, strCommand.c_str(), &ulFpWrite, &ulFpRead, (popen_rlimit_array *)&sSystemLimits, NULL, true, false);
	if (ulCommandPid < 0) {
		hr = MAPI_E_CALL_FAILED;
		goto exit;
	}

	/* Write script input to parser */
	hr = CopyStreamToParser(lpStream, ulFpWrite, ulFpRead, lpstrParsed);
	// NOTE: The function closed the pipes already
	ulFpWrite = ulFpRead = -1;
	if (hr != hrSuccess) {
		if (hr == MAPI_E_NO_SUPPORT)
			m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_INFO, "No parser for attachment: %s (%ls)", strCommand.c_str(), strFilename.c_str());
		else if (hr == MAPI_E_TIMEOUT)
			m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_WARNING, "Timeout while reading from parser for attachment: %s (%ls)", strCommand.c_str(), strFilename.c_str());
		else if (hr == MAPI_E_TOO_BIG)
			m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_WARNING, "Attachment too large: (%ls)", strFilename.c_str());
		else
			m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_WARNING, "Attachment parser write error 0x%08X: %s (%ls)", hr, strCommand.c_str(), strFilename.c_str());
		goto exit;
	}

exit:
	if (ulCommandPid > 0) {
		ulCommandRetval = unix_pclose(ulFpRead, ulFpWrite, ulCommandPid);
		if (ulCommandRetval < 0) {
			m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_ERROR, "Failed to stop attachment parser: %s (%ls)", strCommand.c_str(), strFilename.c_str());
		} else {
			if (WIFSIGNALED(ulCommandRetval) && (WTERMSIG(ulCommandRetval) == SIGINT)) {
				/* Operation timed out, although this is a problem we might have read valid data */
				m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_WARNING, "Attachment parser timeout: %s (%ls)",
											  strCommand.c_str(), strFilename.c_str());
			} else if (WIFSIGNALED(ulCommandRetval)) {
				/* Parser canceled because of a signal, although this is a problem we might have read valid data */
				m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_WARNING, "Attachment parser signaled (%d): %s (%ls)",
											  WTERMSIG(ulCommandRetval), strCommand.c_str(), strFilename.c_str());
			} else if (WEXITSTATUS(ulCommandRetval) != EXIT_SUCCESS) {
				/* Parser exited with error message */
				m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_WARNING, "Attachment parser failed (%d): %s (%ls)",
											  WEXITSTATUS(ulCommandRetval), strCommand.c_str(), strFilename.c_str());
				hr = MAPI_E_CALL_FAILED;
			}
		}
	}

	return hr;
}

HRESULT ECIndexImporterAttachment::ParseEmbeddedAttachment(folderid_t folder, docid_t doc, unsigned int version, ECSerializer *lpSerializer, ECIndexDB *lpIndex)
{
	HRESULT hr = hrSuccess;

	hr = m_lpIndexer->ParseStream(folder, doc, version, lpSerializer, lpIndex, false, NULL);
	if (hr != hrSuccess)
		goto exit;

exit:
	return hr;
}

HRESULT ECIndexImporterAttachment::ParseValueAttachment(folderid_t folder, docid_t doc, unsigned int version, IStream *lpStream,
														tstring &strMimeTag, tstring &strExtension, tstring &strFilename,
														std::wstring *lpstrParsed, ECIndexDB *lpIndex)
{
	HRESULT hr = hrSuccess;
	std::string command;
	std::wstring wparsed;

	command.assign(m_strCommand + " ");

	if (!strMimeTag.empty() && strMimeTag.compare(_T("application/octet-stream")) != 0) {
		string tmp = trim(convert_to<string>(strMimeTag), "\r\n ");
		size_t pos = tmp.find_first_of('/');
		if (pos != std::string::npos) {
			set<string, stricmp_comparison>::iterator i = m_lpThreadData->m_setMimeFilter.find(string(tmp,0,pos));
			if (i != m_lpThreadData->m_setMimeFilter.end()) {
				m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_DEBUG, "Skipping filtered attachment mimetype: %s for %ls", tmp.c_str(), strFilename.c_str());
				hr = MAPI_E_INVALID_OBJECT;
				goto exit;
			}
		}
		command.append("mime '");
		command.append(forcealnum(tmp, "/.-"));
		command.append("'");
	} else if (!strExtension.empty()) {
		// this string mostly does not exist
		string tmp = trim(convert_to<string>(strExtension), "\r\n ");
		set<string, stricmp_comparison>::iterator i = m_lpThreadData->m_setExtFilter.find(string(tmp,1)); // skip dot in extension find
		if (i != m_lpThreadData->m_setExtFilter.end()) {
			m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_DEBUG, "Skipping filtered attachment extension: %s for %ls", tmp.c_str(), strFilename.c_str());
			hr = MAPI_E_INVALID_OBJECT;
			goto exit;
		}
		command.append("ext '");
		command.append(forcealnum(tmp, "."));
		command.append("'");
	} else if (!strFilename.empty()) {
		std::string tmp = trim(convert_to<string>(strFilename), "\r\n ");
		size_t pos = tmp.find_last_of('.');
		if (pos == std::string::npos)
			goto exit;

		
		// skip dot in find
		set<string, stricmp_comparison>::iterator i = m_lpThreadData->m_setExtFilter.find(string(tmp, pos+1));
		if (i != m_lpThreadData->m_setExtFilter.end()) {
			m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_DEBUG, "Skipping filtered attachment extension: %ls", strFilename.c_str());
			hr = MAPI_E_INVALID_OBJECT;
			goto exit;
		}

		command.append("ext '");
		command.append(forcealnum(string(tmp, pos), "."));
		command.append("'");
	} else {
		m_lpThreadData->lpLogger->Log(EC_LOGLEVEL_DEBUG, "Invalid attachment, no mimetag, extension or filename");
		hr = MAPI_E_INVALID_OBJECT;
		goto exit;
	}

	if (strFilename.empty())
		strFilename = _T("<unknown filename>");

	hr = ParseAttachmentCommand(strFilename, command, lpStream, &wparsed);
	if (hr != hrSuccess) {
		/* Non-fatal error, we might have read *some* data before the failure */
		hr = hrSuccess;
	}

    /*
	 * Regardless if attachment parsing succeeds/failed the attachment filename
	 * should still be added to the index.
	 */
	if (!strFilename.empty()) {
		wparsed.append(_T(" "));
		wparsed.append(convert_to<std::wstring>(strFilename));
	}

	hr = lpIndex->AddTerm(folder, doc, PROP_ID(PR_BODY), version, wparsed);
	if (hr != hrSuccess)
		goto exit;

	if (lpstrParsed)
		lpstrParsed->assign(wparsed);

exit:
	return hr;
}

HRESULT ECIndexImporterAttachment::ParseAttachment(folderid_t folder, docid_t doc, unsigned int version, ECSerializer *lpSerializer, ECIndexDB *lpIndex)
{
	HRESULT hr = hrSuccess;
	ECRESULT er = erSuccess;
	IStream *lpStream = NULL;
	LPSPropValue lpProp = NULL;
	tstring strMimeTag;
	tstring strExtension;
	tstring strFilename;
	LARGE_INTEGER lint = {{ 0, 0 }};
	ULONG ulAttachSize = 0;
	ULONG ulMethod = 0;
	ULONG ulProps = 0;
	ULONG i = 0;

	er = lpSerializer->Read(&ulProps, sizeof(ulProps), 1);
	if (er != hrSuccess)
		goto exit;

	for (i = 0; i < ulProps; i++) {
		if (lpProp)
			MAPIFreeBuffer(lpProp);
		lpProp = NULL;

		// always return PT_UNICODE property
		hr = StreamToProperty(lpSerializer, &lpProp, NULL /* No named props supported on attachments */);
		if (hr != hrSuccess) {
			if (hr == MAPI_E_NO_SUPPORT) {
				hr = hrSuccess;
				continue;
			}
			goto exit;
		}

		switch (lpProp->ulPropTag) {
		case PR_ATTACH_METHOD:
			/* Check if we actually want this attachment or not */
			if (lpProp->Value.ul == NO_ATTACHMENT ||
				lpProp->Value.ul == ATTACH_BY_REFERENCE ||
				lpProp->Value.ul == ATTACH_BY_REF_RESOLVE ||
				lpProp->Value.ul == ATTACH_BY_REF_ONLY ||
				lpProp->Value.ul == ATTACH_OLE) {
					hr = MAPI_E_INVALID_OBJECT;
					goto exit;
			}
			ulMethod = lpProp->Value.ul;
			break;
		case PR_ATTACH_SIZE:
			/*
			 * Check for maximum allowed size, if PR_ATTACH_SIZE is not found, we will check later
			 * again based on stream size. But having the correct value here, prevents the overhead
			 * of opening the attachment and loading the attachment stream.
			 */
			if (lpProp->Value.ul > m_lpThreadData->m_ulAttachMaxSize) {
				hr = MAPI_E_TOO_BIG;
				goto exit;
			}
			break;
		case PR_ATTACH_MIME_TAG_W:
			strMimeTag = convert_to<tstring>(lpProp->Value.lpszW);
			break;
		case PR_ATTACH_EXTENSION_W:
			strExtension = convert_to<tstring>(lpProp->Value.lpszW);
			break;
		case PR_ATTACH_LONG_FILENAME_W:
			strFilename = convert_to<tstring>(lpProp->Value.lpszW);
			break;
		default:
			break;
		}
	}

	/* Attachment data is located behind all properties */
	hr = CreateSyncStream(&lpStream);
	if (hr != hrSuccess)
		goto exit;

	er = lpSerializer->Read(&ulAttachSize, sizeof(ulAttachSize), 1);
	if (er != hrSuccess)
		goto exit;

	if (ulAttachSize) {
		/* Copy attachment data into stream */
		while (TRUE) {
			ULONG ulRead = min(m_ulCache, ulAttachSize);
			ULONG ulCopy = ulRead;
			ULONG ulWritten = 0;

			memset(m_lpCache, 0, m_ulCache);

			er = lpSerializer->Read(m_lpCache, 1, ulRead);
			if (er != erSuccess)
				goto exit;

			while (TRUE) {
				hr = lpStream->Write(m_lpCache, ulCopy, &ulWritten);
				if (hr != hrSuccess)
					goto exit;

				assert(ulWritten <= ulCopy);
				if (ulCopy == ulWritten)
					break;
				ulCopy -= ulWritten;
			}

			assert(ulRead <= ulAttachSize);
			if (ulAttachSize == ulRead)
				break;
			ulAttachSize -= ulRead;
		}

		hr = lpStream->Seek(lint, SEEK_SET, NULL);
		if (hr != hrSuccess)
			goto exit;

		switch (ulMethod) {
		case ATTACH_EMBEDDED_MSG:
			hr = ParseEmbeddedAttachment(folder, doc, version, lpSerializer, lpIndex);
			if (hr != hrSuccess)
				goto exit;
			break;
		case ATTACH_BY_VALUE:
		default:
			hr = ParseValueAttachment(folder, doc, version, lpStream, strMimeTag, strExtension, strFilename, NULL, lpIndex);
			if (hr != hrSuccess)
				goto exit;
			break;
		}
	}

	hr = ParseAttachments(folder, doc, version, lpSerializer, lpIndex);
	if (hr != hrSuccess)
		goto exit;

exit:
	/*
	 * Whatever the status of the function, we have to guarentee that the
	 * stream position is pointing to the end of the stream or the next object.
	 */
	if (i != ulProps) {
		hr = StreamToNextObject(lpSerializer, MAPI_ATTACH, ulProps - i - 1 /* minus one since we never break out of the loop without reading the next property */);
	} else if (hr != hrSuccess) {
		// error checking in this function is too agressive, we actually don't know the state of the stream
		// but since most functions never return an error, we can assume when this happens, the attachment has been read
		// but not the subobject (msg-in-msg or 0 for normal attachments)
		// since only the ATTACH_BY_VALUE can return an error in a normal environment, this will always just read a 0 for the number of subobjects.
		hr = StreamToNextObject(lpSerializer, 0); // invalid type, so it doesn't stream subobjects
	}

	if (lpStream)
		lpStream->Release();

	if (lpProp)
		MAPIFreeBuffer(lpProp);

	if (er != erSuccess)
		hr = ZarafaErrorToMAPIError(er);

	return hr;
}

/** 
 * Parse or skip all subobjects. Messages have attachments and recipients, attachments may have a message as subobject.
 * 
 * @param[in] folder folder id
 * @param[in] doc document id
 * @param[in] version document version
 * @param[in] lpSerializer data stream
 * @param[in] lpIndex index class
 * 
 * @return MAPI Error code
 */
HRESULT ECIndexImporterAttachment::ParseAttachments(folderid_t folder, docid_t doc, unsigned int version, ECSerializer *lpSerializer, ECIndexDB *lpIndex)
{
	HRESULT hr = hrSuccess;
	ECRESULT er = erSuccess;
	ULONG ulNumAttachments = 0;
	ULONG ulTmp[2];

	er = lpSerializer->Read(&ulNumAttachments, sizeof(ulNumAttachments), 1);
	if (er != erSuccess)
		goto exit;

	for (ULONG i = 0; i < ulNumAttachments; i++) {
		/* Read Object type and id, but we don't need them */
		er = lpSerializer->Read(ulTmp, sizeof(ulTmp[0]), 2);
		if (er != erSuccess)
			goto exit;

		switch (ulTmp[0]) {
		case MAPI_ATTACH:
			ParseAttachment(folder, doc, version, lpSerializer, lpIndex);
			break;
		case MAPI_MESSAGE:
			ParseEmbeddedAttachment(folder, doc, version, lpSerializer, lpIndex);
			break;
		default:
			/* Subobject is a recipient, skip to next object */
			hr = StreamToNextObject(lpSerializer, ulTmp[0]);
			if (hr != hrSuccess)
				goto exit;
			break;
		}
	}

exit:
	if (er != erSuccess)
		hr = ZarafaErrorToMAPIError(er);

	return hr;
}
