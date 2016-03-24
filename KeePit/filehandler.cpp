/*
* This file is part of KeePit
*
* Copyright (C) 2016 Dan Beavon
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "filehandler.h"

#include <fstream>
#include <string>

using namespace std;

FileHandler::FileHandler()
{
}

void FileHandler::deleteFile(QString filePath)
{
    remove(filePath.toStdString().c_str());
}

char* FileHandler::readFile(QString filePath, std::streampos &size)
{
    ifstream file;
    char * memblock;
    string s1 = filePath.toStdString();
    const char * c = s1.c_str();

    if(!fileExists(c)) {
        //emit error("File does not exist");
        return 0;
    }

    file.open(c, ios::in | ios::binary| ios::ate);
    size = file.tellg();
    memblock = new char[size];
    file.seekg(0, ios::beg);
    file.read(memblock, size);
    file.close();

    return memblock;
}

/*void FileHandler::openFile(QString url, QString password, QString passKey)
{

}*/

bool FileHandler::fileExists(const char* fileName)
{
    ifstream infile(fileName);
    return infile.good();
}
