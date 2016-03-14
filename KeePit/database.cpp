#include "database.h"
#include <iostream>
#include <fstream>
#include <string>
#include <QVariant>
#include <vector>

#include "aes.h"
#include "arrayextensions.h"
#include "readxmlfile.h"
#include "hashedblockstream.h"
#include "compositekey.h"
#include "sha256.h"
#include "salsa20.h"
#include "passwordentry.h"
#include "base64.h"
#include "readkeyfile.h"

using namespace std;

#define FINALKEYSIZE 32
#define MASTERSEEDSIZE 32
#define CYPHERUUIDSIZE 16
#define TRANSFORMSEEDSIZE 32
#define ENCRYPTIONROUNDSSIZE 8
#define ENCRYPTIONIVSIZE 16
#define PROTECTEDSTREAMKEYSIZE 32
#define STREAMSTARTBYTESSIZE 32
#define INNERRANDOMSEEDSIZE 4

char *m_pbMasterSeed; //[32];
char *m_pbCompression;
char *m_cypherUuid; //[16];
char *m_pbTransformSeed;//[32];
char *m_pwDatabaseKeyEncryptionRounds;//[8];
char *m_pbEncryptionIV;//[16];
char *m_pbProtectedStreamKey;//[32];
char *m_pbStreamStartBytes;//[32];
char *m_pbInnerRandomStreamID;//[4];

char m_pbIVSalsa[] = { 0xE8, 0x30, 0x09, 0x4B,
                    0x97, 0x20, 0x5D, 0x2A };

char m_uuidAes[] = {
                        0x31, 0xC1, 0xF2, 0xE6, 0xBF, 0x71, 0x43, 0x50,
                        0xBE, 0x58, 0x05, 0x21, 0x6A, 0xFC, 0x5A, 0xFF };

enum KdbxHeaderFieldID : byte
{
    EndOfHeader = 0,
    Comment = 1,
    CipherID = 2,
    CompressionFlags = 3,
    MasterSeed = 4,
    TransformSeed = 5,
    TransformRounds = 6,
    EncryptionIV = 7,
    ProtectedStreamKey = 8,
    StreamStartBytes = 9,
    InnerRandomStreamID = 10
};

enum DbState {
    closed = 0,
    open = 1
};


DbState m_dbState;
const uint FileSignature1 = 0x9AA2D903;
const uint FileSignatureOld1 = 0x9AA2D903;
const uint FileSignature2 = 0xB54BFB67;
const uint FileSignatureOld2 = 0xB54BFB65;
const uint FileSignaturePreRelease1 = 0x9AA2D903;
const uint FileSignaturePreRelease2 = 0xB54BFB66;
const uint FileVersionCriticalMask = 0xFFFF0000;
const uint FileVersion32 = 0x00030001;

PasswordEntryModel* model;
vector<TreeNode*> dataTree;
vector<TreeNode*> current;
bool foundAny = false;

Database::Database(QObject *parent) :
    QObject(parent)
{    
}

Database::~Database() {
}

void Database::deleteFile(QString filePath) {
    std::remove(filePath.toStdString().c_str());
}

void Database::loadHome() {
    model->removeRows(0, model->rowCount());
    for(uint i=0;i<dataTree.size();i++) {
        model->addPasswordEntry(dataTree[i]->passwordEntry());
    }
}

void Database::search(QString name) {
    foundAny = false;
    searchInternal(name, dataTree);
}

void Database::searchInternal(QString name, vector<TreeNode*> node) {
    for(uint i=0;i<node.size();i++) {
        QString strTitle = node[i]->passwordEntry().title();
        if(strTitle.indexOf(name, 0, Qt::CaseInsensitive) >= 0) {
            if(!foundAny) {
                model->removeRows(0, model->rowCount());
            }
            model->addPasswordEntry(node[i]->passwordEntry());
            foundAny = true;
        }

        if(node[i]->next().size() > 0 ) {
            searchInternal(name, node[i]->next());
        }
    }
}

QString Database::reloadBranch(QString uuid, int entryType)
{
    QString retVal;
    TreeNode* parent = 0;

    if(entryType == Group) {
        // find branch that holds this uuid and reload it into the model
        if(getMyBranch(uuid, dataTree)) {
            model->removeRows(0, model->rowCount());
            for(uint i=0;i<current.size();i++) {
                model->addPasswordEntry(current[i]->passwordEntry());
                if(parent == 0) parent = current[i]->parent();
                if(parent != 0) retVal = parent->passwordEntry().uuid();
            }
        }
    }

    else if( entryType == Entry) {
        // find branch that holds this uuid and reload it into the model
        if(getMyBranch(uuid, dataTree)) {
            model->removeRows(0, model->rowCount());
            for(uint i=0;i<current.size();i++) {
                model->addPasswordEntry(current[i]->passwordEntry());
                if(parent == 0) parent = current[i]->parent();
                if(parent != 0) retVal = parent->passwordEntry().uuid();
            }
        }
    }

    return retVal;
}

void Database::selectBranch(QString uuid)
{
    if(getChildBranch(uuid, dataTree)) {
        model->removeRows(0, model->rowCount());
        for(uint i=0;i<current.size();i++) {
            model->addPasswordEntry(current[i]->passwordEntry());
        }
    }
}

bool Database::getChildBranch(QString uuid, vector<TreeNode*> currentBranch)
{
    TreeNode* node;
    for(uint i=0;i<currentBranch.size();i++) {
        node = currentBranch[i];
        if(node->passwordEntry().uuid() == uuid) {
            current = node->next();
            return true;
        }

        else if(node->next().size() > 0) {
            if(getChildBranch(uuid, node->next())) {
                return true;
            }
        }
    }

    return false;
}

bool Database::getMyBranch(QString uuid, vector<TreeNode*> currentBranch)
{
    TreeNode* node;
    for(uint i=0;i<currentBranch.size();i++) {
        node = currentBranch[i];
        if(node->passwordEntry().uuid() == uuid) {
            current = currentBranch;
            return true;
        }

        else if(node->next().size() > 0) {
            if(getMyBranch(uuid, node->next())) {
                return true;
            }
        }
    }

    return false;
}

PasswordEntryModel* Database::createModel()
{
    model = new PasswordEntryModel();    

    return model;
}

void Database::closeFile() {
    if(m_dbState != open) {
        return;
    }

    // Close the file by clearing all memory items
    // This will mean we have to push the memory items up to member variables
    model->removeRows(0, model->rowCount());
    dataTree.clear();
    current.clear();
    m_dbState = closed;
}

char* Database::readFile(QString url, std::streampos &size) {

    std::ifstream file;
    char * memblock;
    string s1 = url.toStdString();
    const char * c = s1.c_str();

    if(!fileExists(c)) {
        //emit error("File does not exist");
        return 0;
    }

    file.open(c, std::ios::in | std::ios::binary| std::ios::ate);
    size = file.tellg();
    memblock = new char[size];
    file.seekg(0, std::ios::beg);
    file.read(memblock, size);
    file.close();

    return memblock;
}

void Database::openFile(QString url, QString password, QString passKey) {

    if(m_dbState == open) {
        closeFile();
    }

    Aes aes;
    ArrayExtensions ae;
    std::streampos size, passKeySize;
    char* memblock = readFile(url, size);
    if(memblock == 0) {
        emit error("Database does not exist");
        return;
    }

    char* passKeyMemblock;
    bool hasKeyFile = false;
    if(strcmp(passKey.toStdString().c_str(), "") != 0) {
        passKeyMemblock = readFile(passKey, passKeySize);
        if(passKeyMemblock == 0) {
            emit error("Key file does not exist");
            return;
        }

        hasKeyFile = true;
    }

    uint uSig1 = 0, uSig2 = 0, uVersion = 0;  
    uSig1 = loadByte(memblock, 0);
    uSig2 = loadByte(memblock, 4);

    assert(uSig1 != FileSignatureOld1 || uSig2 != FileSignatureOld2);
    if (uSig1 == FileSignatureOld1 && uSig2 == FileSignatureOld2) {
        emit error("Unsupported database format detected");
        return;
    }

    assert(uSig1 == FileSignature1 && uSig2 == FileSignature2);
    if((uSig1 == FileSignature1) && (uSig2 == FileSignature2)) {}
    else if((uSig1 == FileSignaturePreRelease1) && (uSig2 == FileSignaturePreRelease2)) {}
    else {
        emit error("Unknown file signature detected");
        return;
    }

    uVersion = loadByte(memblock, 8);
    if((uVersion & FileVersionCriticalMask) > (FileVersion32 & FileVersionCriticalMask))
    {
        emit error("Unsupported file version detected");
        return;
    }

    uint offset = 12;
    bool endOfHeaderReached = false;
    bool readError = false;
   // QString errorMessage;
    while(true)
    {
        // Add try catch to here
        try {
            uint bytesRead = readHeaderField(memblock, offset, &endOfHeaderReached, &readError);

            if(readError) {
                return;
            }

            offset += bytesRead;

            if(endOfHeaderReached) {
                break;
            }
        }
        catch(const std::exception &e) {
            emit error("Error parsing database");
            return;
        }
    }

    // Create a SHA256 hash of the header data
    char header[offset];
    for(uint i = 0; i<=offset; i++) {
        header[i] = memblock[i];
    }

    // Not sure yet what this is used for, store it and move on
    // In KeePass its a byte here a std::string
    //std::string hash = generateSHA256Hash(header, offset);
    vector<char> v(header, header + sizeof header / sizeof header[0]);
    SHA256 sha256;
    vector<char> hash = sha256.computeHash(v);

    assert(m_pbMasterSeed != NULL);
    if(m_pbMasterSeed == NULL) {
        emit error("Invalid master seed length");
        return;
    }

    // Generate master key
    // Read keyfile if necessary
    ReadKeyFile readKeyFile;
    vector<char> vKeyFileData;
    if(hasKeyFile) {
        vKeyFileData = readKeyFile.read(passKeyMemblock, (int)passKeySize);
    }

    string stringKey = password.toStdString();
    const byte * key = reinterpret_cast<const byte*>(stringKey.c_str());
    vector<char> vKey = ae.toVector((char*)key, (uint)password.size());
    vector<char> vKeySeed = ae.toVector(m_pbTransformSeed, TRANSFORMSEEDSIZE);
    uint uNumRounds = readBytes(m_pwDatabaseKeyEncryptionRounds, 0, 8);

    CompositeKey* cmpKey = new CompositeKey(vKey, vKeyFileData);
    vector<char> pKey32 = cmpKey->generateKey32(vKeySeed, uNumRounds);    
    delete cmpKey;
    cmpKey = 0;

    vector<char> masterSeed = ae.toVector(m_pbMasterSeed, MASTERSEEDSIZE);
    vector<char> ms;
    ms.reserve( vKeySeed.size() + pKey32.size() ); // preallocate memory
    ms.insert( ms.end(), masterSeed.begin(), masterSeed.end() );
    ms.insert( ms.end(), pKey32.begin(), pKey32.end() );
    vector<char> aesKey = sha256.computeHash(ms);

    if(aesKey.size() != FINALKEYSIZE) {
        emit error("FinalKey creation failed");
        return;
    }

    byte pbAesKey[FINALKEYSIZE];
    for(uint i=0;i<FINALKEYSIZE;i++){
        pbAesKey[i] = aesKey[i];
    }

    uint contentSize = size-offset;
    byte pbFileContent[contentSize];
    for(uint i=0;i<contentSize;i++) {
        pbFileContent[i] = memblock[i+offset];
    }

    string recovered;
    try {
         recovered = aes.decrypt(pbAesKey, FINALKEYSIZE, (byte*)m_pbEncryptionIV, (byte*)pbFileContent, contentSize);
    } catch (CryptoPP::InvalidCiphertext &ex) {
        emit error("Invalid composite key");
        return;
    }

    vector<char> startBytes = ae.toVector((char*)recovered.c_str(), STREAMSTARTBYTESSIZE);
    if(m_pbStreamStartBytes == NULL) {
        emit error("Invalid data read");
        return;
    }

    for(int iStart = 0; iStart<STREAMSTARTBYTESSIZE;iStart++) {
        if(startBytes[iStart] != m_pbStreamStartBytes[iStart]) {
            emit error("Invalid composite key");
            return;
        }
    }

    if(m_pbProtectedStreamKey == NULL) {
        emit error("Invalid protected stream key");
        return;
    }

    vector<char> vStreamKey = ae.toVector(m_pbProtectedStreamKey, PROTECTEDSTREAMKEYSIZE);
    vector<char> vStreamKeyHash = sha256.computeHash(vStreamKey);
    salsa = new Salsa20(vStreamKeyHash, m_pbIVSalsa);

    vector<char> payload;
    uint recoveredOffset = 32;
    for(uint i=recoveredOffset;i<recovered.size(); i++) {
        payload.push_back(recovered[i]);
    }

    assert(recovered.size() > 0);
    vector<char> read;
    assert(read.size() == 0);

    try {
        readPayload(&read, payload);
    } catch(exception &ex) {
        emit error("Could not read payload (incorrect composite key?)");
        return;
    }         

    // We have Xml so we need to parse it. My idea is to convert the entire Xml file into c++ objects and then
    // pass them back a level at a time as requested
    const char* xml = read.data();
    assert(read.size() > 0);
    ReadXmlFile *readXml = new ReadXmlFile(xml, read.size(), salsa);
    dataTree = readXml->GetTopGroup();
    loadHome();    
    m_dbState = open;

    emit success();

    return;
}

void Database::readPayload(vector<char>* read, vector<char> payload) {

    HashedBlockStream *hashedStream = new HashedBlockStream(payload, false, 0, true);

    int readBytes = 0;
    int i=0;
    int sz = 1024;
    do
     {
        readBytes = hashedStream->Read(read, (i*sz), sz);
        i++;
    } while(readBytes > 0);

    delete hashedStream;
    hashedStream = 0;
    assert (hashedStream == 0);
}

// Returns true if the file exists else false
bool Database::fileExists(const char *fileName)
{
    std::ifstream infile(fileName);
    return infile.good();
}

// Returns the number of bytes read so that we can keep track of our offset.
// If -1 returned then we are finished
uint Database::readHeaderField(char* memblock, int offset, bool* endOfHeaderReached, bool *readError)
{    
    if(memblock == NULL)
    {
        throw std::exception();
    }

    char btFieldID = memblock[offset++];
    ushort uSize = (ushort)readBytes(memblock, offset, 2);
    offset = offset+2;

    char pbData[uSize];
    if(uSize > 0)
    {
        for(int i = 0; i<uSize; i++) {
            pbData[i] = memblock[(offset+i)];
        }
    }

    uint uCompression;
    KdbxHeaderFieldID kdbID = (KdbxHeaderFieldID)btFieldID;
    switch(kdbID)
    {
        case EndOfHeader:
            *endOfHeaderReached = true; // Returning 0 indicates end of header (no bytes read). Figure out how to pass an out parameter so we can check that for an end flag
            break;

        case CipherID:
            // Only aes cipher supported, check and throw exception if not correct
            m_cypherUuid = new char[uSize];
            copy(pbData, pbData + uSize, m_cypherUuid);
            if(!equal(m_uuidAes, m_cypherUuid, uSize)) {
                throw std::exception();
            }
            break;

        case CompressionFlags:
            // Compression not supported (check pbData is 0 and throw exception if not)
            // Put in version 2 if version 1 ever gets off the ground
            m_pbCompression = new char[uSize];
            copy(pbData, pbData + uSize, m_pbCompression);
            uCompression = loadByte(m_pbCompression, 0);
            if(uCompression != 0) {
                *readError = true;
                emit error("Compressed Databases are not currently supported");
                return 0;
            }
            break;

        case MasterSeed:
           // Not sure what this is doing so, move on and come back
           m_pbMasterSeed = new char[uSize];
           copy(pbData, pbData + uSize, m_pbMasterSeed);
           // CryptoRandom.Instance.AddEntropy(pbData);
            break;

        case TransformSeed:
            // Not sure what this is doing so, move on and come back
            m_pbTransformSeed = new char[uSize];
            copy(pbData, pbData + uSize, m_pbTransformSeed);
           // CryptoRandom.Instance.AddEntropy(pbData);
            break;

        case TransformRounds:
            m_pwDatabaseKeyEncryptionRounds  = new char[uSize];
            copy(pbData, pbData + uSize, m_pwDatabaseKeyEncryptionRounds);
            break;

        case EncryptionIV:
            m_pbEncryptionIV  = new char[uSize];
            copy(pbData, pbData + uSize, m_pbEncryptionIV);
            break;

        case ProtectedStreamKey:
            // Not sure what this is doing so, move on and come back
           m_pbProtectedStreamKey = new char[uSize];
           copy(pbData, pbData + uSize, m_pbProtectedStreamKey);
           // CryloadHome()ptoRandom.Instance.AddEntropy(pbData);
            break;

        case StreamStartBytes:
            m_pbStreamStartBytes = new char[uSize];
            copy(pbData, pbData + uSize, m_pbStreamStartBytes);
            break;

        case InnerRandomStreamID:
            m_pbInnerRandomStreamID = new char[uSize];
            copy(pbData, pbData + uSize, m_pbInnerRandomStreamID);
           // SetInnerRandomStreamID(pbData);
            break;

        default:
            throw std::exception();
            break;
    }

    return (uSize + 3);
}

uint Database::readBytes(char* memblock, int offset, uint size)
{
    uint result = 0;
    byte tmp[size];

    for(uint i = 0; i<size; i++) {
        tmp[i] = memblock[(i+offset)];
    }

    for(int i = (size - 1); i>=0; i--) {
        result = (result << 8) + (unsigned char)tmp[i];
    }

    return result;
}

uint Database::loadByte(char* memblock, int offset)
{
    return readBytes(memblock, offset, 4);
}

bool Database::equal(char* type1, char* type2, uint size) {
    for(uint i = 0; i<size;i++) {
        if(type1[i] != type2[i]) {
            return false;
        }
    }

    return true;
}