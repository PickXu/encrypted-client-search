/**
 * Copyright (C) 2007 Stefan Buettcher. All rights reserved.
 * This is free software with ABSOLUTELY NO WARRANTY.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA
 **/

/**
 * Implementation of the UpdateQuery class.
 *
 * author: Stefan Buettcher
 * created: 2004-10-11
 * changed: 2009-02-01
 **/


#include "updatequery.h"
#include <glob.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include <string.h>
#include <fstream>
#include <dirent.h>
#include <sys/stat.h>
#include "../index/index_types.h"
#include "../misc/all.h"


static const char *COMMANDS[9] = {
	"addfile",
	"adddir",
	"compact",
	"removefile",
	"rename",
	"sync",
	"update",
	"updateattr",
	NULL
};


/**
 * Removes any ".." and symbolic links from the given path.
 **/
static char *normalizePath(char *path) {
	if (path == NULL)
		return NULL;
	char *oldPath = path;
	if ((path[0] != 0) && (path[0] != '/')) {
		char curDir[PATH_MAX];
		if (getcwd(curDir, sizeof(curDir)) != NULL)
			path = evaluateRelativePathName(curDir, path);
		else
			return NULL;
	}
	char realPath[PATH_MAX];
	if (realpath(path, realPath) == NULL) {
		if (path != oldPath)
			free(path);
		return NULL;
	}
	if (path != oldPath)
		free(path);
	return duplicateString(realPath);
} // end of normalizePath(char*)

bool is_file(const char* path) {
	struct stat buf;
	stat(path, &buf);
	return S_ISREG(buf.st_mode);
}

bool is_dir(const char* path) {
	struct stat buf;
	stat(path, &buf);
	return S_ISDIR(buf.st_mode);
}
void UpdateQuery::traverse_and_addfile(const char* path, Index *index,const char **modifiers)
{
	if(is_dir(path))
	{
		DIR *pDIR;
		struct dirent *entry;
		if(pDIR = opendir(path))
		{
			while(entry = readdir(pDIR))
			{
				if(strcmp(entry->d_name,".") != 0 && strcmp(entry->d_name,"..") != 0)
				{
					char *np = concatenateStrings(path,"/",entry->d_name);
					traverse_and_addfile(np,index,modifiers);
					free(np);
				}

			}
			closedir(pDIR);
		}
	}
	else if (is_file(path))
	{
		char *event = concatenateStrings("WRITE\t", path);
		if (modifiers[0] != 0) {
			char *temp = concatenateStrings(event, "\t0\t1\t", modifiers[0]);
			free(event);
			event = temp;
		}
		statusCode = index->notify(event);
		if (statusCode != RESULT_SUCCESS){
			std::cout << "Traverse to " << path;
			std::cout << " Error: " << statusCode;
			std::cout << std::endl;
			printErrorMessage(statusCode, returnString);
		}
		free(event);
	}
	else
	{
		std::cout << "Invalid file: " << path << std::endl;
	}
}	

UpdateQuery::UpdateQuery(Index *index, const char *command, const char **modifiers, const char *body,
		uid_t userID, int memoryLimit) {
	struct stat buf;
	this->index = index;
	statusCode = RESULT_SUCCESS;
	returnString[0] = 0;
	processModifiers(modifiers);

	if ((userID != Index::SUPERUSER) && (userID != index->getOwner())) {
		statusCode = ERROR_ACCESS_DENIED;
		return;
	}

	if (index->readOnly) {
		statusCode = ERROR_READ_ONLY;
		return;
	}

	int len = strlen(body);
	if ((body[0] == 0) &&
	    (strcasecmp(command, "compact") != 0) &&
	    (strcasecmp(command, "sync") != 0)) {
		statusCode = ERROR_SYNTAX_ERROR;
		strcpy(returnString, "Argument missing");
		return;
	}
	char *args = duplicateString(body);

	if (strcasecmp(command, "addfile") == 0) {
		// add a new file to the index
		indexUserID = -1;
		if (args == NULL) {
			statusCode = ERROR_SYNTAX_ERROR;
			strcpy(returnString, "No path given");
			free(args);
			return;
		}

		char *path = duplicateAndTrim(args);
		free(args);

		if ((strchr(path, '*') != NULL) || (strchr(path, '?') != NULL)) {
			bool mustReleaseLock = getLock();
			glob_t globData;
			globData.gl_offs = 0;
			int successCount = 0, totalCount = 0;
			if (glob(path, 0, NULL, &globData) == 0) {
				for (int i = 0; i < globData.gl_pathc; i++) {
					char *normalized = normalizePath(globData.gl_pathv[i]);
					char *event = concatenateStrings("WRITE\t", normalized);
					free(normalized);
					if (modifiers[0] != 0) {
						char *temp = concatenateStrings(event, "\t0\t1\t", modifiers[0]);
						free(event);
						event = temp;
					}
					totalCount++;
					if (index->notify(event) == RESULT_SUCCESS)
						successCount++;
					free(event);
				}
				statusCode = RESULT_SUCCESS;
				sprintf(returnString, "Ok. %d/%d files added", successCount, totalCount);
			}
			else {
				statusCode = ERROR_INTERNAL_ERROR;
				printErrorMessage(statusCode, returnString);
			}
			free(path);
			globfree(&globData);
			if (mustReleaseLock)
				releaseLock();
		}
		else {
			char *temp = normalizePath(path);
			free(path);
			path = temp;
			if (path == NULL) {
				statusCode = ERROR_NO_SUCH_FILE;
				strcpy(returnString, "Invalid path (unable to resolve)");
				return;
			}
			char *event = concatenateStrings("WRITE\t", path);
			free(path);
			if (modifiers[0] != 0) {
				char *temp = concatenateStrings(event, "\t0\t1\t", modifiers[0]);
				free(event);
				event = temp;
			}
			statusCode = index->notify(event);
			if (statusCode != RESULT_SUCCESS)
				printErrorMessage(statusCode, returnString);
			free(event);
		}
		return;
	} // end @addfile

	if(strcasecmp(command, "adddir") == 0) {
		// add files in a directory to the index
		indexUserID = -1;
		if (args == NULL) {
			statusCode = ERROR_SYNTAX_ERROR;
			strcpy(returnString, "No path given");
			free(args);
			return;
		}

		char *path = duplicateAndTrim(args);
		free(args);

		char *temp = normalizePath(path);
		free(path);

		path = temp;
		if (path == NULL) {
			statusCode = ERROR_NO_SUCH_FILE;
			strcpy(returnString, "Invalid path (unable to resolve)");
			return;
		}
		traverse_and_addfile(path,index,modifiers);

		return;
	} // end @adddir

	if (strcasecmp(command, "removefile") == 0) {
		char *temp = normalizePath(args);
		free(args);
		if (temp == NULL) {
			statusCode = ERROR_NO_SUCH_FILE;
			strcpy(returnString, "Invalid path (unable to resolve)");
			return;
		}
		char *event = concatenateStrings("UNLINK\t", temp);
		free(temp);
		statusCode = index->notify(event);
		free(event);
		return;
	} // end @removefile

	if (strcasecmp(command, "updateattr") == 0) {
		char *temp = normalizePath(args);
		free(args);
		if (temp == NULL) {
			statusCode = ERROR_NO_SUCH_FILE;
			strcpy(returnString, "Invalid path (unable to resolve)");
			return;
		}
		args = temp;
		if (stat(args, &buf) != 0)
			statusCode = ERROR_NO_SUCH_FILE;
		else if ((S_ISDIR(buf.st_mode)) || (S_ISREG(buf.st_mode))) {
			char *event = concatenateStrings("CHMOD\t", args);
			statusCode = index->notify(event);
			free(event);
		}
		else
			statusCode = ERROR_NO_SUCH_FILE;
		free(args);
		return;
	} // end @updateattr

	if (strcasecmp(command, "rename") == 0) {
		StringTokenizer *tok = new StringTokenizer(args, " \t");
		char *event = duplicateString("RENAME");
		while (tok->hasNext()) {
			char *path = normalizePath(tok->nextToken());
			if (path == NULL) {
				statusCode = ERROR_NO_SUCH_FILE;
				strcpy(returnString, "Invalid path (unable to resolve)");
				delete tok;
				free(event);
				free(args);
				return;
			}
			char *temp = concatenateStrings(event, "\t", path);
			free(path);
			free(event);
			event = temp;
		}
		delete tok;
		statusCode = index->notify(event);
		free(event);
		free(args);
		return;
	} // end @rename

	if (strcasecmp(command, "update") == 0) {
		statusCode = index->notify(args);
		free(args);
		return;
	} // end @update

	if (strcasecmp(command, "compact") == 0) {
		statusCode = ERROR_INTERNAL_ERROR;
		strcpy(returnString, "Not implemented");
		free(args);
		return;
	} // end @compact
	
	if (strcasecmp(command, "sync") == 0) {
		index->sync();
		statusCode = STATUS_OK;
		strcpy(returnString, "Synced");
		return;
	} // end @sync

} // end of UpdateQuery(Index*, char*, char**, char*, uid_t)


UpdateQuery::~UpdateQuery() {
} // end of ~UpdateQuery()


bool UpdateQuery::isValidCommand(const char *command) {
	for (int i = 0; COMMANDS[i] != NULL; i++)
		if (strcasecmp(command, COMMANDS[i]) == 0)
			return true;
	return false;
} // end of isValidCommand(char*)


bool UpdateQuery::parse() {
	return true;
} // end of parse()


bool UpdateQuery::getNextLine(char *line) {
	return false;
} // end of getNextLine(char*)


bool UpdateQuery::getStatus(int *code, char *description) {
	*code = statusCode;
	if (returnString[0] == 0)
		printErrorMessage(statusCode, returnString);
	sprintf(description, "%s.", returnString);
	return true;
} // end of getStatus(int*, char*)


int UpdateQuery::getType() {
	return QUERY_TYPE_UPDATE;
}



