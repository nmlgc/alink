#include "alink.h"
#include "pe.h"

CSWITCHENTRY PESwitches[]={
	{"dll",0,"Generate a DLL instead of an EXE"},
	{"base",1,"Set image Base"},
	{"stack",2,"Set stack sizes"},
	{"stackcommitsize",1,"Set stack commit size"},
	{"stacksize",1,"Set total stack size"},
	{"heap",2,"Set heap sizes"},
	{"heapcommitsize",1,"Set heap commit size"},
	{"heapsize",1,"Set total heap size"},
	{"objectalign",1,"Set object alignment"},
	{"filealign",1,"Set file alignment"},
	{"subsys",1,"Set subsystem"},
	{"osver",1,"Set OS version"},
	{"subsysver",1,"Set subsystem version"},
	{"stub",1,"Set MSDOS stub file to use"},
	{"reloc",0,"Put relocation info in output file"},
	{"debug",0,"Include debug info in output file"},
	{NULL,0,NULL}
};

char PEExtension[]=".EXE";

static UCHAR defaultStub[]={
	0x4D,0x5A,0x6C,0x00,0x01,0x00,0x00,0x00,
	0x02,0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,
	0x00,0x00,0x00,0x00,0x11,0x00,0x00,0x00,
	0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x57,0x69,0x6E,0x33,0x32,0x20,0x50,0x72,
	0x6F,0x67,0x72,0x61,0x6D,0x21,0x0D,0x0A,
	0x24,0xB4,0x09,0xBA,0x00,0x01,0xCD,0x21,
	0xB4,0x4C,0xCD,0x21,0x00,0x00,0x00,0x00
};

#define defaultStubSize (sizeof(defaultStub))

struct lineref
{
	PCHAR filename;
	UINT num;
	UINT section;
	UINT ofs;
};

typedef struct lineref LINEREF,*PLINEREF;

struct region
{
	UINT start;
	UINT end;
};

typedef struct region REGION,*PREGION;

static PLINEREF debugLines=NULL;
static UINT debugLineCount=0;

static BOOL isDll=FALSE;
static BOOL relocsRequired=FALSE;
static BOOL debugRequired=FALSE;
static UINT imageBase=0x400000;
static UINT stackSize=0x100000;
static UINT stackCommitSize=0x1000;
static UINT heapSize=0x100000;
static UINT heapCommitSize=0x1000;
static UINT objectAlign=0x1000;
static UINT fileAlign=0x200;
static USHORT subSystem=PE_SUBSYS_WINDOWS;
static UCHAR subSysMajor=4;
static UCHAR subSysMinor=0;
static UCHAR osMajor=1;
static UCHAR osMinor=0;
static UCHAR userMajor=0;
static UCHAR userMinor=0;
static PUCHAR stub=defaultStub;
static UINT stubSize=defaultStubSize;
static USHORT thisCpu=PE_INTEL386;

static PSEG importSeg,exportSeg,relocSeg,resourceSeg,debugSeg,debugDir;
static UINT impSegNum,expSegNum,relSegNum,resSegNum,debSegNum;

static BOOL parseVersion(PCHAR str,UINT *major,UINT *minor)
{
	UINT numchars;
	return (
		sscanf(str,"%lu.%lu%ln",major,minor,&numchars)==2
		&& str[numchars]=='\0'
		&& (*major<65536) && (*minor<65536)
	);
}

BOOL PEInitialise(PSWITCHPARAM sp)
{
	UINT i,j;
	PCHAR end;

	for(;sp && sp->name;++sp)
	{
		if(!strcmp(sp->name,"objectalign"))
		{
			errno=0;
			i=strtoul(sp->params[0],&end,0);
			if(errno || (*end))
			{
				addError("Invalid number (%s) for objectalign parameter",sp->params[0]);
				return FALSE;
			}
			if(getBitCount(i)!=1)
			{
				addError("Cannot have non-power-of-2 as objectalign parameter");
				return FALSE;
			}
			objectAlign=i;
		}
		else if(!strcmp(sp->name,"filealign"))
		{
			errno=0;
			i=strtoul(sp->params[0],&end,0);
			if(errno || (*end))
			{
				addError("Invalid number (%s) for filealign parameter",sp->params[0]);
				return FALSE;
			}
			if(getBitCount(i)!=1)
			{
				addError("Cannot have non-power-of-2 as filealign parameter");
				return FALSE;
			}
			if(i>0x10000)
			{
				addError("File alignment must be 64k or less");
				return FALSE;
			}

			fileAlign=i;
		}
		else if(!strcmp(sp->name,"subsys"))
		{
			if(!strcmp(sp->params[0],"con")
			   || !strcmp(sp->params[0],"console")
			   || !strcmp(sp->params[0],"char"))
			{
				subSystem=PE_SUBSYS_CONSOLE;
			}
			else if(!strcmp(sp->params[0],"gui")
			        || !strcmp(sp->params[0],"win")
			        || !strcmp(sp->params[0],"windows"))
			{
				subSystem=PE_SUBSYS_WINDOWS;
			}
			else if(!strcmp(sp->params[0],"posix"))
			{
				subSystem=PE_SUBSYS_POSIX;
			}
			else if(!strcmp(sp->params[0],"native"))
			{
				subSystem=PE_SUBSYS_NATIVE;
			}
			else
			{
				addError("Invalid subsystem %s",sp->params[0]);
				return FALSE;
			}
		}
		else if(!strcmp(sp->name,"base"))
		{
			errno=0;
			i=strtoul(sp->params[0],&end,0);
			if(errno || (*end))
			{
				addError("Invalid number (%s) for base parameter",sp->params[0]);
				return FALSE;
			}
			if(i&0xffff)
			{
				addError("Image base must be a multiple of 64k");
				return FALSE;
			}
			imageBase=i;
		}
		else if(!strcmp(sp->name,"dll"))
		{
			isDll=TRUE;
			relocsRequired=TRUE;
			strcpy(PEExtension,".DLL");
		}
		else if(!strcmp(sp->name,"reloc"))
		{
			relocsRequired=TRUE;
		}
		else if(!strcmp(sp->name,"debug"))
		{
			debugRequired=TRUE;
		}
		else if(!strcmp(sp->name,"stacksize"))
		{
			errno=0;
			i=strtoul(sp->params[0],&end,0);
			if(errno || (*end))
			{
				addError("Invalid number (%s) for stacksize parameter",sp->params[0]);
				return FALSE;
			}
			stackSize=i;
		}
		else if(!strcmp(sp->name,"stack"))
		{
			errno=0;
			i=strtoul(sp->params[0],&end,0);
			if(errno || (*end))
			{
				addError("Invalid number (%s) for stack parameter",sp->params[0]);
				return FALSE;
			}
			stackSize=i;
			i=strtoul(sp->params[1],&end,0);
			if(errno || (*end))
			{
				addError("Invalid number (%s) for stack parameter",sp->params[1]);
				return FALSE;
			}
			stackCommitSize=i;
		}
		else if(!strcmp(sp->name,"stackcommitsize"))
		{
			errno=0;
			i=strtoul(sp->params[0],&end,0);
			if(errno || (*end))
			{
				addError("Invalid number (%s) for stackcommitsize parameter",sp->params[0]);
				return FALSE;
			}
			stackCommitSize=i;
		}
		else if(!strcmp(sp->name,"heapsize"))
		{
			errno=0;
			i=strtoul(sp->params[0],&end,0);
			if(errno || (*end))
			{
				addError("Invalid number (%s) for heapsize parameter",sp->params[0]);
				return FALSE;
			}
			heapSize=i;
		}
		else if(!strcmp(sp->name,"heapcommitsize"))
		{
			errno=0;
			i=strtoul(sp->params[0],&end,0);
			if(errno || (*end))
			{
				addError("Invalid number (%s) for heapcommitsize parameter",sp->params[0]);
				return FALSE;
			}
			heapCommitSize=i;
		}
		else if(!strcmp(sp->name,"heap"))
		{
			errno=0;
			i=strtoul(sp->params[0],&end,0);
			if(errno || (*end))
			{
				addError("Invalid number (%s) for heap parameter",sp->params[0]);
				return FALSE;
			}
			heapSize=i;
			i=strtoul(sp->params[1],&end,0);
			if(errno || (*end))
			{
				addError("Invalid number (%s) for heap parameter",sp->params[1]);
				return FALSE;
			}
			heapCommitSize=i;
		}
		else if(!strcmp(sp->name,"subsysver"))
		{
			if(!parseVersion(sp->params[0],&i,&j))
			{
				addError("Invalid subsystem version number %s",sp->params[0]);
				return FALSE;
			}
			subSysMajor=i;
			subSysMinor=j;
		}
		else if(!strcmp(sp->name,"osver"))
		{
			if(!parseVersion(sp->params[0],&i,&j))
			{
				addError("Invalid OS version number %s",sp->params[0]);
				return FALSE;
			}
			osMajor=i;
			osMinor=j;
		}
		else if(!strcmp(sp->name,"stub"))
		{
			getStub(sp->params[0],&stub,&stubSize);
		}
	}

	if(stackSize<stackCommitSize)
	{
		addError("Stack size less than commit size, setting stack to commit size");
		stackSize=stackCommitSize;
	}
	if(heapSize<heapCommitSize)
	{
		addError("Heap size less than commit size, setting heap to commit size");
		heapSize=heapCommitSize;
	}
	if(objectAlign<fileAlign)
	{
		addError("Object alignment less than file alignment, setting file alignment to object alignment");
		fileAlign=objectAlign;
	}
	if(objectAlign<4096)
	{
		if(objectAlign!=fileAlign)
		{
			addError("File alignment doesn't match object alignment, and less than page size, setting fileAlign to objectAlign");
			fileAlign=objectAlign;
		}
	}

	globalSegs=checkRealloc(globalSegs,(globalSegCount+3)*sizeof(PSEG));

	importSeg=createSection(".idata",NULL,NULL,NULL,0,1);
	importSeg->use32=TRUE;
	importSeg->initdata=TRUE;
	importSeg->read=TRUE;
	importSeg->internal=TRUE;
	impSegNum=globalSegCount;
	globalSegs[impSegNum]=importSeg;

	exportSeg=createSection(".edata",NULL,NULL,NULL,0,1);
	exportSeg->use32=TRUE;
	exportSeg->initdata=TRUE;
	exportSeg->read=TRUE;
	exportSeg->internal=TRUE;
	expSegNum=globalSegCount+1;
	globalSegs[expSegNum]=exportSeg;

	resourceSeg=createSection(".rsrc",NULL,NULL,NULL,0,1);
	resourceSeg->use32=TRUE;
	resourceSeg->initdata=TRUE;
	resourceSeg->shared=TRUE;
	resourceSeg->read=TRUE;
	resourceSeg->internal=TRUE;
	resSegNum=globalSegCount+2;
	globalSegs[resSegNum]=resourceSeg;
	globalSegCount+=3;

	if(relocsRequired)
	{
		globalSegs=checkRealloc(globalSegs,(globalSegCount+1)*sizeof(PSEG));
		relocSeg=createSection(".reloc",NULL,NULL,NULL,0,1);
		relocSeg->use32=TRUE;
		relocSeg->initdata=TRUE;
		relocSeg->discardable=TRUE;
		relocSeg->read=TRUE;
		relocSeg->internal=TRUE;
		relSegNum=globalSegCount;
		globalSegs[relSegNum]=relocSeg;
		globalSegCount+=1;
	}
	else
	{
		relocSeg=NULL;
		relSegNum=0;
	}

	if(debugRequired)
	{
		globalSegs=checkRealloc(globalSegs,(globalSegCount+1)*sizeof(PSEG));
		debugSeg=createSection(".debug",NULL,NULL,NULL,0,1);
		debugSeg->use32=TRUE;
		debugSeg->initdata=TRUE;
		debugSeg->read=TRUE;
		debugSeg->internal=TRUE;
		debSegNum=globalSegCount;
		globalSegs[debSegNum]=debugSeg;
		globalSegCount+=1;
	}
	else
	{
		debugSeg=NULL;
		debSegNum=0;
	}


	defaultUse32=TRUE;

	return TRUE;
}

static PSEG createPEHeader(void)
{
	PSEG h,lastSeg,codestart=NULL,codeend=NULL,datastart=NULL,dataend=NULL;
	PUCHAR headbuf,objbuf;
	UINT headbufSize;
	PDATABLOCK headBlock,objectBlock;
	UINT i,j,k;
	time_t now;

	h=createSection("PEHeader",NULL,NULL,NULL,0,1);
	h->internal=TRUE;
	h->use32=TRUE;
	headbufSize=PE_HEADBUF_SIZE;
	headBlock=createDataBlock(NULL,0,headbufSize,8);
	addData(h,headBlock);
	headbuf=headBlock->data;

	headbuf[PE_SIGNATURE]='P';
	headbuf[PE_SIGNATURE+1]='E';
	headbuf[PE_SIGNATURE+2]=0;
	headbuf[PE_SIGNATURE+3]=0;

	Set16(&headbuf[PE_MACHINEID],thisCpu);

	time(&now);
	Set32(&headbuf[PE_DATESTAMP],now);

	Set16(&headbuf[PE_HDRSIZE],PE_OPTIONAL_HEADER_SIZE);

	if(isDll)
	{
		i=PE_FILE_EXECUTABLE | PE_FILE_32BIT | PE_FILE_LIBRARY;
	}
	else
	{
		i=PE_FILE_EXECUTABLE | PE_FILE_32BIT;
	}

	Set16(&headbuf[PE_FLAGS],i);
	Set16(&headbuf[PE_MAGIC],PE_MAGICNUM);

	headbuf[PE_LMAJOR]=ALINK_MAJOR;
	headbuf[PE_LMINOR]=ALINK_MINOR;

	Set32(&headbuf[PE_IMAGEBASE],imageBase);
	Set32(&headbuf[PE_OBJECTALIGN],objectAlign);
	Set32(&headbuf[PE_FILEALIGN],fileAlign);
	Set16(&headbuf[PE_OSMAJOR],osMajor);
	Set16(&headbuf[PE_OSMINOR],osMinor);
	Set16(&headbuf[PE_USERMAJOR],userMajor);
	Set16(&headbuf[PE_USERMINOR],userMinor);
	Set16(&headbuf[PE_SUBSYSMAJOR],subSysMajor);
	Set16(&headbuf[PE_SUBSYSMINOR],subSysMinor);
	Set16(&headbuf[PE_SUBSYSTEM],subSystem);
	Set16(&headbuf[PE_NUMRVAS],PE_NUM_VAS);
	Set32(&headbuf[PE_HEAPSIZE],heapSize);
	Set32(&headbuf[PE_HEAPCOMMSIZE],heapCommitSize);
	Set32(&headbuf[PE_STACKSIZE],stackSize);
	Set32(&headbuf[PE_STACKCOMMSIZE],stackCommitSize);

	if(gotstart)
	{
		h->relocs=checkRealloc(h->relocs,(h->relocCount+1)*sizeof(RELOC));
		h->relocs[h->relocCount]=startaddr;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_RVA;
		h->relocs[h->relocCount].ofs=PE_ENTRYPOINT;
		h->relocCount++;

		if(isDll) /* if library */
		{
			/* flag that entry point should always be called */
			headbuf[PE_DLLFLAGS]=0xf;
			headbuf[PE_DLLFLAGS+1]=0;
		}
	}
	else
	{
		if(!isDll)
			diagnostic(DIAG_BASIC,"Warning: No entry point specified\n");
	}

	if(relocSeg)
	{
		for(i=0;i<globalSegCount && globalSegs[i]!=relocSeg;++i);
		if(i!=globalSegCount)
		{
			for(j=i;j<(globalSegCount-1);++j)
			{
				globalSegs[j]=globalSegs[j+1];
			}
			globalSegs[j]=relocSeg;
		}
	}

	for(i=0,j=0,lastSeg=NULL;i<globalSegCount;++i)
	{
		if(!globalSegs[i]) continue;
		if(globalSegs[i]->absolute) continue;
		/* empty segments don't go in object table */
		if(!globalSegs[i]->length) continue;
		if(globalSegs[i]->code)
		{
			if(!codestart) codestart=globalSegs[i];
			codeend=globalSegs[i];
		}
		if(globalSegs[i]->initdata)
		{
			if(!datastart) datastart=globalSegs[i];
			dataend=globalSegs[i];
		}


		++j;
		lastSeg=globalSegs[i];
		globalSegs[i]->section=j;
		if(globalSegs[i]->align < objectAlign)
			globalSegs[i]->align=objectAlign;
		objectBlock=createDataBlock(NULL,0,PE_OBJECTENTRY_SIZE,8);
		addData(h,objectBlock);
		objbuf=objectBlock->data;
		for(k=0;k<8;++k)
		{
			if(globalSegs[i]->name && globalSegs[i]->name[k])
			{
				objbuf[PE_OBJECT_NAME+k]=globalSegs[i]->name[k];
			}
			else break;
		}

		k=0;
		if(globalSegs[i]->code)
			k |= WINF_CODE;
		if(globalSegs[i]->initdata)
			k |= WINF_INITDATA;
		if(globalSegs[i]->uninitdata)
			k |= WINF_UNINITDATA;
		if(globalSegs[i]->discardable)
			k |= WINF_DISCARDABLE;
		if(globalSegs[i]->shared)
			k |= WINF_SHARED;
		if(globalSegs[i]->read)
			k |= WINF_READABLE;
		if(globalSegs[i]->write)
			k |= WINF_WRITEABLE;
		if(globalSegs[i]->execute)
			k |= WINF_EXECUTE;
		Set32(&objbuf[PE_OBJECT_FLAGS],k);

		k=globalSegs[i]->length;
		k+=objectAlign-1;
		k&=0xffffffff-(objectAlign-1);
		Set32(&objbuf[PE_OBJECT_VIRTSIZE],k);

		k=getInitLength(globalSegs[i]);
		if(k)
		{
			Set32(&objbuf[PE_OBJECT_RAWSIZE],k);

			h->relocs=checkRealloc(h->relocs,(h->relocCount+1)*sizeof(RELOC));
			h->relocs[h->relocCount].tseg=globalSegs[i];
			h->relocs[h->relocCount].disp=0;
			h->relocs[h->relocCount].fseg=NULL;
			h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
			h->relocs[h->relocCount].rtype=REL_OFS32;
			h->relocs[h->relocCount].base=REL_FILEPOS;
			h->relocs[h->relocCount].ofs=objectBlock->offset+PE_OBJECT_RAWPTR;
			h->relocCount++;
		}

		h->relocs=checkRealloc(h->relocs,(h->relocCount+1)*sizeof(RELOC));
		h->relocs[h->relocCount].tseg=globalSegs[i];
		h->relocs[h->relocCount].disp=0;
		h->relocs[h->relocCount].fseg=NULL;
		h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_RVA;
		h->relocs[h->relocCount].ofs=objectBlock->offset+PE_OBJECT_VIRTADDR;
		h->relocCount++;

		if(j==1)
		{
			h->relocs=checkRealloc(h->relocs,(h->relocCount+1)*sizeof(RELOC));
			h->relocs[h->relocCount].tseg=globalSegs[i];
			h->relocs[h->relocCount].disp=0;
			h->relocs[h->relocCount].fseg=NULL;
			h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
			h->relocs[h->relocCount].rtype=REL_OFS32;
			h->relocs[h->relocCount].base=REL_FILEPOS;
			h->relocs[h->relocCount].ofs=PE_HEADERSIZE;
			h->relocCount++;
		}
	}

	Set16(&headbuf[PE_NUMOBJECTS],j);

	if(codestart)
	{
		h->relocs=checkRealloc(h->relocs,(h->relocCount+2)*sizeof(RELOC));
		h->relocs[h->relocCount].tseg=codestart;
		h->relocs[h->relocCount].disp=0;
		h->relocs[h->relocCount].fseg=NULL;
		h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_RVA;
		h->relocs[h->relocCount].ofs=PE_CODEBASE;
		h->relocCount++;

		h->relocs[h->relocCount].tseg=codeend;
		h->relocs[h->relocCount].disp=codeend->length;
		h->relocs[h->relocCount].fseg=codestart;
		h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_FRAME;
		h->relocs[h->relocCount].ofs=PE_CODESIZE;
		h->relocCount++;
	}

	if(datastart)
	{
		h->relocs=checkRealloc(h->relocs,(h->relocCount+2)*sizeof(RELOC));
		h->relocs[h->relocCount].tseg=datastart;
		h->relocs[h->relocCount].disp=0;
		h->relocs[h->relocCount].fseg=NULL;
		h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_RVA;
		h->relocs[h->relocCount].ofs=PE_DATABASE;
		h->relocCount++;

		h->relocs[h->relocCount].tseg=dataend;
		h->relocs[h->relocCount].disp=dataend->length;
		h->relocs[h->relocCount].fseg=datastart;
		h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_FRAME;
		h->relocs[h->relocCount].ofs=PE_INITDATASIZE;
		h->relocCount++;
	}

	if(importSeg)
	{
		h->relocs=checkRealloc(h->relocs,(h->relocCount+1)*sizeof(RELOC));
		h->relocs[h->relocCount].tseg=importSeg;
		h->relocs[h->relocCount].disp=0;
		h->relocs[h->relocCount].fseg=NULL;
		h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_RVA;
		h->relocs[h->relocCount].ofs=PE_IMPORTRVA;
		h->relocCount++;

		Set32(&headbuf[PE_IMPORTSIZE],importSeg->length);
	}
	if(exportSeg)
	{
		h->relocs=checkRealloc(h->relocs,(h->relocCount+1)*sizeof(RELOC));
		h->relocs[h->relocCount].tseg=exportSeg;
		h->relocs[h->relocCount].disp=0;
		h->relocs[h->relocCount].fseg=NULL;
		h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_RVA;
		h->relocs[h->relocCount].ofs=PE_EXPORTRVA;
		h->relocCount++;

		Set32(&headbuf[PE_EXPORTSIZE],exportSeg->length);
	}
	if(relocSeg)
	{
		h->relocs=checkRealloc(h->relocs,(h->relocCount+1)*sizeof(RELOC));
		h->relocs[h->relocCount].tseg=relocSeg;
		h->relocs[h->relocCount].disp=0;
		h->relocs[h->relocCount].fseg=NULL;
		h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_RVA;
		h->relocs[h->relocCount].ofs=PE_FIXUPRVA;
		h->relocCount++;

		Set32(&headbuf[PE_FIXUPSIZE],relocSeg->length);
	}
	if(resourceSeg)
	{
		h->relocs=checkRealloc(h->relocs,(h->relocCount+1)*sizeof(RELOC));
		h->relocs[h->relocCount].tseg=resourceSeg;
		h->relocs[h->relocCount].disp=0;
		h->relocs[h->relocCount].fseg=NULL;
		h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_RVA;
		h->relocs[h->relocCount].ofs=PE_RESOURCERVA;
		h->relocCount++;

		Set32(&headbuf[PE_RESOURCESIZE],resourceSeg->length);
	}
	if(debugSeg && debugDir)
	{
		h->relocs=checkRealloc(h->relocs,(h->relocCount+1)*sizeof(RELOC));
		h->relocs[h->relocCount].tseg=debugDir;
		h->relocs[h->relocCount].disp=0;
		h->relocs[h->relocCount].fseg=NULL;
		h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_RVA;
		h->relocs[h->relocCount].ofs=PE_DEBUGRVA;
		h->relocCount++;

		Set32(&headbuf[PE_DEBUGSIZE],debugDir->length);
	}


	if(!j)
	{
		addError("No segments to output\n");
		return h;
	}
	else
	{
		k=lastSeg->length;
		k+=objectAlign-1;
		k&=0xffffffff-(objectAlign-1);

		Set32(&headbuf[PE_IMAGESIZE],k);

		h->relocs=checkRealloc(h->relocs,(h->relocCount+1)*sizeof(RELOC));
		h->relocs[h->relocCount].tseg=lastSeg;
		h->relocs[h->relocCount].disp=0;
		h->relocs[h->relocCount].fseg=NULL;
		h->relocs[h->relocCount].fext=h->relocs[h->relocCount].text=NULL;
		h->relocs[h->relocCount].rtype=REL_OFS32;
		h->relocs[h->relocCount].base=REL_RVA;
		h->relocs[h->relocCount].ofs=PE_IMAGESIZE;
		h->relocCount++;
	}

	return h;
}

static BOOL buildPEImports(void)
{
	UINT i,j,k;
	PIMPDLL dllList=NULL;
	UINT dllCount=0;
	PIMPENTRY ie;
	PSEG dllDir,lookup,thunk,hintName,nameTable;
	PDATABLOCK dirEntry,lookupEntry,thunkEntry,nameEntry,hintEntry;

	for(i=0;i<globalSymbolCount;++i)
	{
		if(globalSymbols[i]->type!=PUB_IMPORT) continue; /* ignore non-imports */
		if(!globalSymbols[i]->refCount) continue; /* ignore unrefenced symbols */
		for(k=0;k<dllCount;k++)
		{
			if(!strcmp(dllList[k].name,globalSymbols[i]->dllname)) break;
		}
		/* if dll not in list, add to it */
		if(k==dllCount)
		{
			dllCount++;
			dllList=(PIMPDLL)checkRealloc(dllList,dllCount*sizeof(IMPDLL));
			dllList[k].name=globalSymbols[i]->dllname;
			dllList[k].entry=NULL;
			dllList[k].entryCount=0;
		}
		for(j=0,ie=NULL;j<dllList[k].entryCount;++j)
		{
			ie=dllList[k].entry+j;
			if((!ie->name && !globalSymbols[i]->impname && (ie->ordinal==globalSymbols[i]->ordinal))
			   || (ie->name && globalSymbols[i]->impname && !strcmp(ie->name,globalSymbols[i]->impname)))
				break;
		}
		if(j==dllList[k].entryCount)
		{
			dllList[k].entryCount++;
			dllList[k].entry=checkRealloc(dllList[k].entry,dllList[k].entryCount*sizeof(IMPENTRY));
			dllList[k].entry[j].name=globalSymbols[i]->impname;
			dllList[k].entry[j].ordinal=globalSymbols[i]->ordinal;
			dllList[k].entry[j].publist=NULL;
			dllList[k].entry[j].pubcount=0;
			ie=dllList[k].entry+j;
		}
		ie->publist=checkRealloc(ie->publist,(ie->pubcount+1)*sizeof(PSYMBOL));
		ie->publist[ie->pubcount]=globalSymbols[i];
		ie->pubcount++;
	}

	if(!dllCount) return TRUE; /* no imports, all is OK */

	dllDir=createSection("DLL Directory",NULL,NULL,NULL,0,4);
	dllDir->internal=TRUE;
	dllDir->use32=TRUE;
	addSeg(importSeg,dllDir);
	hintName=createSection("Hint-Name table",NULL,NULL,NULL,0,4);
	hintName->internal=TRUE;
	hintName->use32=TRUE;
	addSeg(importSeg,hintName);
	nameTable=createSection("DLL Name table",NULL,NULL,NULL,0,4);
	nameTable->internal=TRUE;
	nameTable->use32=TRUE;
	addSeg(importSeg,nameTable);

	for(i=0;i<dllCount;++i)
	{
		dirEntry=createDataBlock(NULL,0,PE_IMPORTDIRENTRY_SIZE,1);
		addData(dllDir,dirEntry);
		nameEntry=createDataBlock(dllList[i].name,0,strlen(dllList[i].name)+1,1);
		addData(nameTable,nameEntry);
		lookup=createSection(dllList[i].name,"DLL lookup table",NULL,NULL,0,4);
		lookup->internal=TRUE;
		lookup->use32=TRUE;
		addSeg(importSeg,lookup);
		thunk=createSection(dllList[i].name,"DLL thunk table",NULL,NULL,0,4);
		thunk->internal=TRUE;
		thunk->use32=TRUE;
		addSeg(importSeg,thunk);

		for(j=0;j<dllList[i].entryCount;++j)
		{
			lookupEntry=createDataBlock(NULL,0,4,1);
			addData(lookup,lookupEntry);
			thunkEntry=createDataBlock(NULL,0,4,1);
			addData(thunk,thunkEntry);

			if(!dllList[i].entry[j].name)
			{
				k=dllList[i].entry[j].ordinal | PE_ORDINAL_FLAG;
				Set32(lookupEntry->data,k);
				Set32(thunkEntry->data,k);
			}
			else
			{
				hintEntry=createDataBlock(NULL,0,strlen(dllList[i].entry[j].name)+3,2);
				addData(hintName,hintEntry);
				Set16(hintEntry->data,dllList[i].entry[j].ordinal);
				strcpy(hintEntry->data+2,dllList[i].entry[j].name);

				lookup->relocs=checkRealloc(lookup->relocs,(lookup->relocCount+1)*sizeof(RELOC));
				lookup->relocs[lookup->relocCount].ofs=lookupEntry->offset;
				lookup->relocs[lookup->relocCount].disp=hintEntry->offset;
				lookup->relocs[lookup->relocCount].tseg=hintName;
				lookup->relocs[lookup->relocCount].fseg=NULL;
				lookup->relocs[lookup->relocCount].fext=lookup->relocs[lookup->relocCount].text=NULL;
				lookup->relocs[lookup->relocCount].base=REL_RVA;
				lookup->relocs[lookup->relocCount].rtype=REL_OFS32;
				lookup->relocCount++;
				thunk->relocs=checkRealloc(thunk->relocs,(thunk->relocCount+1)*sizeof(RELOC));
				thunk->relocs[thunk->relocCount].ofs=thunkEntry->offset;
				thunk->relocs[thunk->relocCount].disp=hintEntry->offset;
				thunk->relocs[thunk->relocCount].tseg=hintName;
				thunk->relocs[thunk->relocCount].fseg=NULL;
				thunk->relocs[thunk->relocCount].fext=thunk->relocs[thunk->relocCount].text=NULL;
				thunk->relocs[thunk->relocCount].base=REL_RVA;
				thunk->relocs[thunk->relocCount].rtype=REL_OFS32;
				thunk->relocCount++;
			}

			for(k=0;k<dllList[i].entry[j].pubcount;++k)
			{
				dllList[i].entry[j].publist[k]->seg=thunk;
				dllList[i].entry[j].publist[k]->ofs=thunkEntry->offset;
			}
		}
		/* add NULL terminator for lookup list */
		lookupEntry=createDataBlock(NULL,0,4,1);
		addData(lookup,lookupEntry);
		/* add NULL terminator for thunk list */
		thunkEntry=createDataBlock(NULL,0,4,1);
		addData(thunk,thunkEntry);


		dllDir->relocs=checkRealloc(dllDir->relocs,(dllDir->relocCount+3)*sizeof(RELOC));
		dllDir->relocs[dllDir->relocCount].ofs=dirEntry->offset+PE_IMPORT_NAME;
		dllDir->relocs[dllDir->relocCount].disp=nameEntry->offset;
		dllDir->relocs[dllDir->relocCount].tseg=nameTable;
		dllDir->relocs[dllDir->relocCount].fseg=NULL;
		dllDir->relocs[dllDir->relocCount].fext=dllDir->relocs[dllDir->relocCount].text=NULL;
		dllDir->relocs[dllDir->relocCount].base=REL_RVA;
		dllDir->relocs[dllDir->relocCount].rtype=REL_OFS32;
		dllDir->relocCount++;
		dllDir->relocs[dllDir->relocCount].ofs=dirEntry->offset+PE_IMPORT_LOOKUP;
		dllDir->relocs[dllDir->relocCount].disp=0;
		dllDir->relocs[dllDir->relocCount].tseg=lookup;
		dllDir->relocs[dllDir->relocCount].fseg=NULL;
		dllDir->relocs[dllDir->relocCount].fext=dllDir->relocs[dllDir->relocCount].text=NULL;
		dllDir->relocs[dllDir->relocCount].base=REL_RVA;
		dllDir->relocs[dllDir->relocCount].rtype=REL_OFS32;
		dllDir->relocCount++;
		dllDir->relocs[dllDir->relocCount].ofs=dirEntry->offset+PE_IMPORT_THUNK;
		dllDir->relocs[dllDir->relocCount].disp=0;
		dllDir->relocs[dllDir->relocCount].tseg=thunk;
		dllDir->relocs[dllDir->relocCount].fseg=NULL;
		dllDir->relocs[dllDir->relocCount].fext=dllDir->relocs[dllDir->relocCount].text=NULL;
		dllDir->relocs[dllDir->relocCount].base=REL_RVA;
		dllDir->relocs[dllDir->relocCount].rtype=REL_OFS32;
		dllDir->relocCount++;
	}
	/* add a final NULL entry to finish DLL directory */
	dirEntry=createDataBlock(NULL,0,PE_IMPORTDIRENTRY_SIZE,1);
	addData(dllDir,dirEntry);

	return TRUE;
}

static USHORT wtolower(USHORT w)
{
	if(w>255) return w;
	return tolower(w);
}

static int resourceCompare(const void *x1,const void *x2)
{
	PRESOURCE r1,r2;
	UINT i;
	USHORT c1,c2;

	r1=(PRESOURCE)x1;
	r2=(PRESOURCE)x2;

	/* compare types */
	if(!r1->typename)
	{
		if(!r2->typename)
		{
			/* no names, so compare numbers */
			if(r1->typeid<r2->typeid) return -1;
			if(r1->typeid>r2->typeid) return 1;
		}
		/* numbers are always greater than names */
		else
			return 1;
	}
	else if(!r2->typename) return -1; /* numbers are greater than names */

	if(r1->typename && r2->typename)
	{
		/* compare strings */
		for(i=0,c1=c2=TRUE;c1&&c2;++i)
		{
			c1=wtolower(r1->typename[2*i]+(r1->typename[2*i+1]<<8));
			c2=wtolower(r2->typename[2*i]+(r2->typename[2*i+1]<<8));
			if(c1<c2) return -1;
			if(c2>c1) return +1;
		}
	}

	/* equal types => compare names */
	if(!r1->name)
	{
		if(!r2->name)
		{
			/* no names, so compare numbers */
			if(r1->id<r2->id) return -2;
			if(r1->id>r2->id) return 2;
		}
		/* numbers are always greater than names */
		else
			return 2;
	}
	else if(!r2->name) return -2; /* numbers are greater than names */

	if(r1->name && r2->name)
	{
		/* compare strings */
		for(i=0,c1=c2=TRUE;c1&&c2;++i)
		{
			c1=wtolower(r1->name[2*i]+(r1->name[2*i+1]<<8));
			c2=wtolower(r2->name[2*i]+(r2->name[2*i+1]<<8));
			if(c1<c2) return -2;
			if(c2>c1) return +2;
		}
	}

	/* same name and type, compare languages */
	/* no names, so compare numbers */
	if(r1->languageid<r2->languageid) return -3;
	if(r1->languageid>r2->languageid) return 3;
	return 0;
}


static BOOL buildPEResources(void)
{
	UINT i,j;
	UINT compres;
	PSEG typeSeg,idSeg,langSeg,dataSeg,realDataSeg,nameSeg;
	PDATABLOCK typeHeader=NULL,idHeader=NULL,langHeader=NULL;
	PDATABLOCK typeEntry,idEntry,langEntry,dataEntry,nameEntry,realData;
	UINT typenameCount=0,typeidCount=0,nameCount=0,idCount=0,langCount=0;
	time_t now;

	if(!globalResourceCount) return TRUE;

	for(i=0;i<globalResourceCount;++i)
	{
		if(!globalResources[i].is32)
		{
			addError("16-bit resource present");
			return FALSE;
		}
	}

	/* sort resources into order */
	qsort(globalResources,globalResourceCount,sizeof(RESOURCE),resourceCompare);

	/* get time */
	time(&now);

	typeSeg=createSection("Type Directory",NULL,NULL,NULL,0,4);
	typeSeg->internal=TRUE;
	typeSeg->use32=TRUE;
	typeSeg->shared=TRUE;
	addSeg(resourceSeg,typeSeg);

	idSeg=createSection("ID Directories",NULL,NULL,NULL,0,4);
	idSeg->internal=TRUE;
	idSeg->use32=TRUE;
	idSeg->shared=TRUE;
	addSeg(resourceSeg,idSeg);

	langSeg=createSection("Language Directories",NULL,NULL,NULL,0,4);
	langSeg->internal=TRUE;
	langSeg->use32=TRUE;
	langSeg->shared=TRUE;
	addSeg(resourceSeg,langSeg);

	dataSeg=createSection("Resource Data Entries",NULL,NULL,NULL,0,4);
	dataSeg->internal=TRUE;
	dataSeg->use32=TRUE;
	dataSeg->shared=TRUE;
	addSeg(resourceSeg,dataSeg);

	realDataSeg=createSection("Resource Data",NULL,NULL,NULL,0,4);
	realDataSeg->internal=TRUE;
	realDataSeg->use32=TRUE;
	realDataSeg->shared=TRUE;
	addSeg(resourceSeg,realDataSeg);

	nameSeg=createSection("Resource Names",NULL,NULL,NULL,0,4);
	nameSeg->internal=TRUE;
	nameSeg->use32=TRUE;
	nameSeg->shared=TRUE;
	addSeg(resourceSeg,nameSeg);

	typeHeader=createDataBlock(NULL,0,PE_RES_DIRHDR_SIZE,4);
	addData(typeSeg,typeHeader);

	Set32(&typeHeader->data[PE_RES_DIRHDR_TIME],now);

	for(i=0;i<globalResourceCount;++i)
	{
		if(i)
			compres=abs(resourceCompare(globalResources+i-1,globalResources+i));
		else compres=0;
		if(i && !compres)
		{
			addError("Duplicate resource entry found");
			return FALSE;
		}

		if(!i || compres==1)
		{
			/* OK, new type */
			/* store counts of IDs */
			if(idHeader)
			{
				Set16(&idHeader->data[PE_RES_DIRHDR_NUMNAMES],nameCount);

				Set16(&idHeader->data[PE_RES_DIRHDR_NUMIDS],idCount);
			}
			/* create new id directory */
			idHeader=createDataBlock(NULL,0,PE_RES_DIRHDR_SIZE,4);
			addData(idSeg,idHeader);
			nameCount=idCount=0;

			Set32(&idHeader->data[PE_RES_DIRHDR_TIME],now);

			/* new entry in type directory */
			typeEntry=createDataBlock(NULL,0,PE_RES_DIRENTRY_SIZE,4);
			addData(typeSeg,typeEntry);

			/* allocate space for a name */
			if(globalResources[i].typename)
			{
				j=wstrlen(globalResources[i].typename);
				nameEntry=createDataBlock(NULL,0,2*(j+1),2);
				addData(nameSeg,nameEntry);
				Set16(nameEntry->data,j);
				memcpy(nameEntry->data+2,globalResources[i].typename,2*j);

				typeEntry->data[PE_RES_DIRENTRY_ID+3]=0x80;

				typeSeg->relocs=checkRealloc(typeSeg->relocs,(typeSeg->relocCount+1)*sizeof(RELOC));
				typeSeg->relocs[typeSeg->relocCount].ofs=typeEntry->offset+PE_RES_DIRENTRY_ID;
				typeSeg->relocs[typeSeg->relocCount].base=REL_FRAME;
				typeSeg->relocs[typeSeg->relocCount].tseg=nameSeg;
				typeSeg->relocs[typeSeg->relocCount].fseg=resourceSeg;
				typeSeg->relocs[typeSeg->relocCount].disp=nameEntry->offset;
				typeSeg->relocs[typeSeg->relocCount].text=typeSeg->relocs[typeSeg->relocCount].fext=NULL;
				typeSeg->relocs[typeSeg->relocCount].rtype=REL_OFS32;
				typeSeg->relocCount++;
				typenameCount++;
			}
			else
			{
				Set32(&typeEntry->data[PE_RES_DIRENTRY_ID],globalResources[i].typeid);
				typeidCount++;
			}
			typeEntry->data[PE_RES_DIRENTRY_RVA+3]=0x80; /* set high bit of RVA, to indicate subdirectory */

			/* add a reloc to get RVA of ID directory*/
			typeSeg->relocs=checkRealloc(typeSeg->relocs,(typeSeg->relocCount+1)*sizeof(RELOC));
			typeSeg->relocs[typeSeg->relocCount].ofs=typeEntry->offset+PE_RES_DIRENTRY_RVA;
			typeSeg->relocs[typeSeg->relocCount].base=REL_FRAME;
			typeSeg->relocs[typeSeg->relocCount].tseg=idSeg;
			typeSeg->relocs[typeSeg->relocCount].fseg=resourceSeg;
			typeSeg->relocs[typeSeg->relocCount].disp=idHeader->offset;
			typeSeg->relocs[typeSeg->relocCount].text=typeSeg->relocs[typeSeg->relocCount].fext=NULL;
			typeSeg->relocs[typeSeg->relocCount].rtype=REL_OFS32;
			typeSeg->relocCount++;
		}

		if(!i || compres <=2)
		{
			/* OK, new type */
			/* store counts of languages */
			if(langHeader)
			{
				Set16(&langHeader->data[PE_RES_DIRHDR_NUMIDS],langCount);
			}
			/* create new lang directory */
			langHeader=createDataBlock(NULL,0,PE_RES_DIRHDR_SIZE,4);
			addData(langSeg,langHeader);
			langCount=0;

			Set32(&langHeader->data[PE_RES_DIRHDR_TIME],now);

			/* new entry in id directory */
			idEntry=createDataBlock(NULL,0,PE_RES_DIRENTRY_SIZE,4);
			addData(idSeg,idEntry);

			/* allocate space for a name */
			if(globalResources[i].name)
			{
				j=wstrlen(globalResources[i].name);
				nameEntry=createDataBlock(NULL,0,2*(j+1),2);
				addData(nameSeg,nameEntry);
				Set16(nameEntry->data,j);
				memcpy(nameEntry->data+2,globalResources[i].name,2*j);

				idEntry->data[PE_RES_DIRENTRY_ID+3]=0x80;

				idSeg->relocs=checkRealloc(idSeg->relocs,(idSeg->relocCount+1)*sizeof(RELOC));
				idSeg->relocs[idSeg->relocCount].ofs=idEntry->offset+PE_RES_DIRENTRY_ID;
				idSeg->relocs[idSeg->relocCount].base=REL_FRAME;
				idSeg->relocs[idSeg->relocCount].tseg=nameSeg;
				idSeg->relocs[idSeg->relocCount].fseg=resourceSeg;
				idSeg->relocs[idSeg->relocCount].disp=nameEntry->offset;
				idSeg->relocs[idSeg->relocCount].text=idSeg->relocs[idSeg->relocCount].fext=NULL;
				idSeg->relocs[idSeg->relocCount].rtype=REL_OFS32;
				idSeg->relocCount++;
				nameCount++;
			}
			else
			{
				Set32(&idEntry->data[PE_RES_DIRENTRY_ID],globalResources[i].id);
				idCount++;
			}
			idEntry->data[PE_RES_DIRENTRY_RVA+3]=0x80; /* set high bit of RVA, to indicate subdirectory */

			/* add a reloc to get RVA of LANG directory*/
			idSeg->relocs=checkRealloc(idSeg->relocs,(idSeg->relocCount+1)*sizeof(RELOC));
			idSeg->relocs[idSeg->relocCount].ofs=idEntry->offset+PE_RES_DIRENTRY_RVA;
			idSeg->relocs[idSeg->relocCount].base=REL_FRAME;
			idSeg->relocs[idSeg->relocCount].tseg=langSeg;
			idSeg->relocs[idSeg->relocCount].fseg=resourceSeg;
			idSeg->relocs[idSeg->relocCount].disp=langHeader->offset;
			idSeg->relocs[idSeg->relocCount].text=idSeg->relocs[idSeg->relocCount].fext=NULL;
			idSeg->relocs[idSeg->relocCount].rtype=REL_OFS32;
			idSeg->relocCount++;
		}

		/* new entry in id directory */
		langEntry=createDataBlock(NULL,0,PE_RES_DIRENTRY_SIZE,4);
		addData(langSeg,langEntry);
		Set32(&langEntry->data[PE_RES_DIRENTRY_ID],globalResources[i].languageid);
		langCount++;

		/* new data entry */
		dataEntry=createDataBlock(NULL,0,PE_RES_DATAENTRY_SIZE,4);
		addData(dataSeg,dataEntry);

		Set32(&dataEntry->data[PE_RES_DATAENTRY_LENGTH],globalResources[i].length);

		realData=createDataBlock(globalResources[i].data,0,globalResources[i].length,4);
		addData(realDataSeg,realData);

		/* add a reloc to get RVA of data entry */
		langSeg->relocs=checkRealloc(langSeg->relocs,(langSeg->relocCount+1)*sizeof(RELOC));
		langSeg->relocs[langSeg->relocCount].ofs=langEntry->offset+PE_RES_DIRENTRY_RVA;
		langSeg->relocs[langSeg->relocCount].base=REL_FRAME;
		langSeg->relocs[langSeg->relocCount].tseg=dataSeg;
		langSeg->relocs[langSeg->relocCount].fseg=resourceSeg;
		langSeg->relocs[langSeg->relocCount].disp=dataEntry->offset;
		langSeg->relocs[langSeg->relocCount].text=langSeg->relocs[langSeg->relocCount].fext=NULL;
		langSeg->relocs[langSeg->relocCount].rtype=REL_OFS32;
		langSeg->relocCount++;

		/* add a reloc to get RVA of actual data */
		dataSeg->relocs=checkRealloc(dataSeg->relocs,(dataSeg->relocCount+1)*sizeof(RELOC));
		dataSeg->relocs[dataSeg->relocCount].ofs=dataEntry->offset+PE_RES_DATAENTRY_RVA;
		dataSeg->relocs[dataSeg->relocCount].base=REL_RVA;
		dataSeg->relocs[dataSeg->relocCount].tseg=realDataSeg;
		dataSeg->relocs[dataSeg->relocCount].fseg=resourceSeg;
		dataSeg->relocs[dataSeg->relocCount].disp=realData->offset;
		dataSeg->relocs[dataSeg->relocCount].text=dataSeg->relocs[dataSeg->relocCount].fext=NULL;
		dataSeg->relocs[dataSeg->relocCount].rtype=REL_OFS32;
		dataSeg->relocCount++;
	}

	if(idHeader)
	{
		Set16(&idHeader->data[PE_RES_DIRHDR_NUMNAMES],nameCount);
		Set16(&idHeader->data[PE_RES_DIRHDR_NUMIDS],idCount);
	}
	if(langHeader)
	{
		Set16(&langHeader->data[PE_RES_DIRHDR_NUMIDS],langCount);
	}
	Set16(&typeHeader->data[PE_RES_DIRHDR_NUMNAMES],typenameCount);
	Set16(&typeHeader->data[PE_RES_DIRHDR_NUMIDS],typeidCount);

	return TRUE;
}

static int exportCompare(PPEXPORTREC e1,PPEXPORTREC e2)
{
	if((*e1)->exp_name && (*e2)->exp_name) return strcmp((*e1)->exp_name,(*e2)->exp_name);
	if((*e1)->exp_name && !(*e2)->exp_name) return -1;
	if(!(*e1)->exp_name && (*e2)->exp_name) return +1;
	if((*e1)->ordinal<(*e2)->ordinal) return -1;
	if((*e1)->ordinal==(*e2)->ordinal) return 0;
	return +1;
}


static BOOL buildPEExports(PCHAR name)
{
	UINT i,j;
	UINT minOrd,maxOrd,numNames,numExports;
	PSEG nameTable,addressTable,ordTable,nameList,forwarderList;
	PDATABLOCK addrEntry,nameEntry,ordEntry,realName,forwarderEntry,hdr;
	BOOL isName,isForwarder;
	PCHAR forwarderString;
	time_t now;

	/* nothing to do if no exports */
	if(!globalExportCount) return TRUE;

	hdr=createDataBlock(NULL,0,PE_EXPORT_HEADER_SIZE,4);
	addData(exportSeg,hdr);

	nameTable=createSection("Export name table",NULL,NULL,NULL,0,4);
	nameTable->internal=TRUE;

	addressTable=createSection("Export address table",NULL,NULL,NULL,0,4);
	addressTable->internal=TRUE;

	ordTable=createSection("Export ordinal table",NULL,NULL,NULL,0,4);
	ordTable->internal=TRUE;

	nameList=createSection("Export name list",NULL,NULL,NULL,0,4);
	nameList->internal=TRUE;

	forwarderList=createSection("Export forwarder list",NULL,NULL,NULL,0,4);
	forwarderList->internal=TRUE;

	for(i=0,minOrd=UINT_MAX,maxOrd=0;i<globalExportCount;++i)
	{
		if(globalExports[i]->ordinal)
		{
			if(globalExports[i]->ordinal<minOrd) minOrd=globalExports[i]->ordinal;
			if(globalExports[i]->ordinal>maxOrd) maxOrd=globalExports[i]->ordinal;
		}
	}

	if(maxOrd>=minOrd) /* actually got some ordinal references? */
	{
		i=maxOrd-minOrd+1; /* get number of ordinals */
	}
	else
	{
		minOrd=1; /* if none defined, min is set to 1 */
	}

	qsort(globalExports,globalExportCount,sizeof(PEXPORTREC),(PCOMPAREFUNC)exportCompare);

	numNames=numExports=0;

	for(i=0;i<globalExportCount;++i)
	{
		if(!globalExports[i]->intsym->pubdef)
		{
			addError("Export has no matching internal symbol: %s",globalExports[i]->int_name);
			return FALSE;
		}
		if(globalExports[i]->intsym->pubdef->type==PUB_EXPORT)
		{
			addError("Cannot export a reference to another export: %s",globalExports[i]->int_name);
			return FALSE;
		}
		isForwarder=(globalExports[i]->intsym->pubdef->type==PUB_IMPORT);

		if(globalExports[i]->exp_name && strlen(globalExports[i]->exp_name))
		{
			/* named export */
			if( i && globalExports[i-1]->exp_name && !strcmp(globalExports[i-1]->exp_name,globalExports[i]->exp_name))
			{
				/* same exported name - check details of export */
				/* if exact duplicates, then skip this one */
				if((globalExports[i]->ordinal==globalExports[i-1]->ordinal)
				   && !strcmp(globalExports[i]->int_name,globalExports[i-1]->int_name)) continue;
				/* otherwise, error */
				addError("Two exports with same name %s\n",globalExports[i]->exp_name);
				return FALSE;
			}
			isName=TRUE;
		}
		else
		{
			isName=FALSE;
		}

		numExports++;
		if(!globalExports[i]->ordinal)
		{
			for(j=0,maxOrd=0;j<addressTable->contentCount;++j)
			{
				/* skip if gap before next ordinal entry, and maxOrd is in gap */
				if(addressTable->contentList[j].data->offset>maxOrd) break;
				/* if we find data at the address we are looking for, move search up */
				if(addressTable->contentList[j].data->offset==maxOrd)
					maxOrd+=4;
			}
			globalExports[i]->ordinal=maxOrd/4+minOrd;
		}

		addrEntry=createDataBlock(NULL,(globalExports[i]->ordinal-minOrd)*4,4,1);
		if(addressTable->length<(addrEntry->offset+4))
		{
			addressTable->length=(addrEntry->offset+4);
		}

		addFixedData(addressTable,addrEntry);

		/* add a reloc to fill in this entry */
		addressTable->relocs=checkRealloc(addressTable->relocs,(addressTable->relocCount+1)*sizeof(RELOC));
		addressTable->relocs[addressTable->relocCount].base=REL_RVA;
		addressTable->relocs[addressTable->relocCount].rtype=REL_OFS32;
		addressTable->relocs[addressTable->relocCount].ofs=addrEntry->offset;
		addressTable->relocs[addressTable->relocCount].fext=NULL;
		addressTable->relocs[addressTable->relocCount].fseg=NULL;
		if(isForwarder)
		{
			/* dll-name+"."+terminating null */
			if(strrchr(globalExports[i]->intsym->pubdef->dllname,'.'))
			{
				maxOrd=strrchr(globalExports[i]->intsym->pubdef->dllname,'.')-globalExports[i]->intsym->pubdef->dllname;
			}
			else
			{
				maxOrd=strlen(globalExports[i]->intsym->pubdef->dllname);
			}

			j=maxOrd+2;

			if(globalExports[i]->intsym->pubdef->impname)
			{
				j+=strlen(globalExports[i]->intsym->pubdef->impname);
			}
			else
			{
				j+=6; /* max 6 chars for #num for 16-bit ordinals */
			}
			forwarderString=checkMalloc(j);
			if(globalExports[i]->intsym->pubdef->impname)
			{
				sprintf(forwarderString,"%.*s.%s",(int)maxOrd,globalExports[i]->intsym->pubdef->dllname,globalExports[i]->intsym->pubdef->impname);
			}
			else
			{
				sprintf(forwarderString,"%.*s.#%hi",(int)maxOrd,globalExports[i]->intsym->pubdef->dllname,globalExports[i]->intsym->pubdef->ordinal);
			}

			forwarderEntry=createDataBlock(forwarderString,0,strlen(forwarderString)+1,1);
			addData(forwarderList,forwarderEntry);
			checkFree(forwarderString);
			addressTable->relocs[addressTable->relocCount].tseg=forwarderList;
			addressTable->relocs[addressTable->relocCount].text=NULL;
			addressTable->relocs[addressTable->relocCount].disp=forwarderEntry->offset;
		}
		else
		{
			addressTable->relocs[addressTable->relocCount].tseg=NULL;
			addressTable->relocs[addressTable->relocCount].text=globalExports[i]->intsym;
			addressTable->relocs[addressTable->relocCount].disp=0;
		}

		addressTable->relocCount++;

		if(isName)
		{
			numNames++;

			realName=createDataBlock(globalExports[i]->exp_name,0,strlen(globalExports[i]->exp_name)+1,1);
			addData(nameList,realName);

			nameEntry=createDataBlock(NULL,0,4,1);
			addData(nameTable,nameEntry);

			ordEntry=createDataBlock(NULL,0,2,1);
			addData(ordTable,ordEntry);

			Set16(ordEntry->data,(globalExports[i]->ordinal-minOrd));

			/* add a reloc to fill in the name seg entry */
			nameTable->relocs=checkRealloc(nameTable->relocs,(nameTable->relocCount+1)*sizeof(RELOC));
			nameTable->relocs[nameTable->relocCount].base=REL_RVA;
			nameTable->relocs[nameTable->relocCount].rtype=REL_OFS32;
			nameTable->relocs[nameTable->relocCount].ofs=nameEntry->offset;
			nameTable->relocs[nameTable->relocCount].fext=NULL;
			nameTable->relocs[nameTable->relocCount].fseg=NULL;
			nameTable->relocs[nameTable->relocCount].tseg=nameList;
			nameTable->relocs[nameTable->relocCount].text=NULL;
			nameTable->relocs[nameTable->relocCount].disp=realName->offset;
			nameTable->relocCount++;
		}
	}

	time(&now);

	Set32(&hdr->data[PE_EXPORT_TIME],now);
	Set32(&hdr->data[PE_EXPORT_NUMEXPORTS],numExports);
	Set32(&hdr->data[PE_EXPORT_NUMNAMES],numNames);
	Set32(&hdr->data[PE_EXPORT_ORDINALBASE],minOrd);

	addSeg(exportSeg,nameTable);
	addSeg(exportSeg,addressTable);
	addSeg(exportSeg,ordTable);
	addSeg(exportSeg,nameList);
	addSeg(exportSeg,forwarderList);

	while(strpbrk(name,PATHCHARS))
	{
		name=strpbrk(name,PATHCHARS)+1;
	}

	nameEntry=createDataBlock(name,0,strlen(name)+1,1);
	strupr(nameEntry->data);
	addData(exportSeg,nameEntry);

	/* add relocs to fill in remaining fields in header */
	exportSeg->relocs=checkRealloc(exportSeg->relocs,(exportSeg->relocCount+4)*sizeof(RELOC));
	/* address of DLL name */
	exportSeg->relocs[exportSeg->relocCount].base=REL_RVA;
	exportSeg->relocs[exportSeg->relocCount].rtype=REL_OFS32;
	exportSeg->relocs[exportSeg->relocCount].ofs=hdr->offset+PE_EXPORT_NAMERVA;
	exportSeg->relocs[exportSeg->relocCount].fext=NULL;
	exportSeg->relocs[exportSeg->relocCount].fseg=NULL;
	exportSeg->relocs[exportSeg->relocCount].tseg=exportSeg;
	exportSeg->relocs[exportSeg->relocCount].text=NULL;
	exportSeg->relocs[exportSeg->relocCount].disp=nameEntry->offset;
	exportSeg->relocCount++;
	/* address of name table*/
	exportSeg->relocs[exportSeg->relocCount].base=REL_RVA;
	exportSeg->relocs[exportSeg->relocCount].rtype=REL_OFS32;
	exportSeg->relocs[exportSeg->relocCount].ofs=hdr->offset+PE_EXPORT_NAMETABLE;
	exportSeg->relocs[exportSeg->relocCount].fext=NULL;
	exportSeg->relocs[exportSeg->relocCount].fseg=NULL;
	exportSeg->relocs[exportSeg->relocCount].tseg=nameTable;
	exportSeg->relocs[exportSeg->relocCount].text=NULL;
	exportSeg->relocs[exportSeg->relocCount].disp=0;
	exportSeg->relocCount++;
	/* address of address table*/
	exportSeg->relocs[exportSeg->relocCount].base=REL_RVA;
	exportSeg->relocs[exportSeg->relocCount].rtype=REL_OFS32;
	exportSeg->relocs[exportSeg->relocCount].ofs=hdr->offset+PE_EXPORT_ADDRESSTABLE;
	exportSeg->relocs[exportSeg->relocCount].fext=NULL;
	exportSeg->relocs[exportSeg->relocCount].fseg=NULL;
	exportSeg->relocs[exportSeg->relocCount].tseg=addressTable;
	exportSeg->relocs[exportSeg->relocCount].text=NULL;
	exportSeg->relocs[exportSeg->relocCount].disp=0;
	exportSeg->relocCount++;
	/* address of ordinal table*/
	exportSeg->relocs[exportSeg->relocCount].base=REL_RVA;
	exportSeg->relocs[exportSeg->relocCount].rtype=REL_OFS32;
	exportSeg->relocs[exportSeg->relocCount].ofs=hdr->offset+PE_EXPORT_ORDINALTABLE;
	exportSeg->relocs[exportSeg->relocCount].fext=NULL;
	exportSeg->relocs[exportSeg->relocCount].fseg=NULL;
	exportSeg->relocs[exportSeg->relocCount].tseg=ordTable;
	exportSeg->relocs[exportSeg->relocCount].text=NULL;
	exportSeg->relocs[exportSeg->relocCount].disp=0;
	exportSeg->relocCount++;

	return TRUE;
}

static PSEG getSegFixups(PSEG s)
{
	PSEG r,sr;
	UINT i;
	PDATABLOCK blockHeader=NULL,blockEntry;
	UINT blockCount;
	UINT blockStart;

	if(!s) return NULL;

	r=createSection("relocs",s->name,NULL,NULL,0,4);
	r->internal=TRUE;

	for(i=0;i<s->relocCount;++i)
	{
		if((s->relocs[i].base!=REL_ABS)
		   && (s->relocs[i].base!=REL_DEFAULT)) continue; /* we only need to relocate VAs, not RVAs */

		if(!blockHeader || ((s->relocs[i].ofs-blockStart)>4096))
		{
			if(blockHeader)
			{
				/* align to an even count */
				if(blockCount&1)
				{
					blockEntry=createDataBlock(NULL,0,PE_RELOC_ENTRY_SIZE,1);
					addData(r,blockEntry);
					blockCount++;
				}

				blockCount*=2; /* two bytes per entry */
				blockCount+=8; /* plus eight bytes for the header */

				Set32(&blockHeader->data[PE_RELOC_BLOCKSIZE],blockCount);
			}
			blockStart=s->relocs[i].ofs;
			blockCount=0;
			blockHeader=createDataBlock(NULL,0,PE_RELOC_HEADER_SIZE,4);

			addData(r,blockHeader);
			/* add a reloc entry to get the correct page RVA in the header */
			r->relocs=checkRealloc(r->relocs,(r->relocCount+1)*sizeof(RELOC));
			r->relocs[r->relocCount].rtype=REL_OFS32;
			r->relocs[r->relocCount].base=REL_RVA;
			r->relocs[r->relocCount].disp=s->relocs[i].ofs;
			r->relocs[r->relocCount].fseg=NULL;
			r->relocs[r->relocCount].fext=NULL;
			r->relocs[r->relocCount].tseg=s;
			r->relocs[r->relocCount].text=NULL;
			r->relocs[r->relocCount].ofs=PE_RELOC_PAGERVA+blockHeader->offset;
			r->relocCount++;
		}

		blockEntry=createDataBlock(NULL,0,PE_RELOC_ENTRY_SIZE,1);
		addData(r,blockEntry);

		Set16(blockEntry->data,(s->relocs[i].ofs-blockStart));

		switch(s->relocs[i].rtype)
		{
		case REL_OFS32:
			blockEntry->data[1]|=PE_RELOC_HIGHLOW;
			break;
		case REL_OFS16:
			blockEntry->data[1]|=PE_RELOC_LOW16;
			break;
		default:
			addError("Relocation type not supported\n");
			return FALSE;
		}

		blockCount++;
	}
	if(blockHeader)
	{
		/* align to an even count */
		if(blockCount&1)
		{
			blockEntry=createDataBlock(NULL,0,PE_RELOC_ENTRY_SIZE,1);
			addData(r,blockEntry);
			blockCount++;
		}

		blockCount*=2; /* two bytes per entry */
		blockCount+=8; /* plus eight bytes for the header */

		Set32(&blockHeader->data[PE_RELOC_BLOCKSIZE],blockCount);
	}

	for(i=0;i<s->contentCount;++i)
	{
		if(s->contentList[i].flag!=SEGMENT) continue;
		sr=getSegFixups(s->contentList[i].seg);
		if(sr)
			addSeg(r,sr);
	}


	/* if no relocs then we don't need a structure */
	if(!r->contentCount)
	{
		freeSection(r);
		return NULL;
	}
	return r;
}


static BOOL buildPERelocs(void)
{
	UINT i;
	PSEG r;
	PPSEG rlist=NULL;
	UINT rcount=0;
	PDATABLOCK d;

	/* loop through all sections */
	for(i=0;i<globalSegCount;++i)
	{
		if(!globalSegs[i]) continue;
		/* if relocs needed for that section, add to list */
		r=getSegFixups(globalSegs[i]);
		if(r)
		{
			rlist=checkRealloc(rlist,(rcount+1)*sizeof(PSEG));
			rlist[rcount]=r;
			rcount++;
		}
	}

	for(i=0;i<rcount;++i)
	{
		addSeg(relocSeg,rlist[i]);
	}

	if(!rcount)
	{
		/* if no relocs, create a dummy entry */
		d=createDataBlock(NULL,0,12,4);
		addData(relocSeg,d);
		d->data[PE_RELOC_BLOCKSIZE]=12;
	}

	checkFree(rlist);

	return TRUE;
}

static void getLines(PSEG s,UINT section,UINT shift)
{
	UINT i;

	if(s->mod && s->lineCount)
	{
		debugLines=checkRealloc(debugLines,(debugLineCount+s->lineCount)*sizeof(LINEREF));
		for(i=0;i<s->lineCount;++i)
		{
			debugLines[debugLineCount].filename=s->mod->sourceFiles[s->lines[i].sourceFile-1];
			debugLines[debugLineCount].num=s->lines[i].num;
			debugLines[debugLineCount].section=section;
			debugLines[debugLineCount].ofs=s->lines[i].offset+shift+s->base;

			debugLineCount++;
		}
	}

	for(i=0;i<s->contentCount;++i)
	{
		if(s->contentList[i].flag==SEGMENT)
		{
			getLines(s->contentList[i].seg,section,shift+s->base);
		}
	}
}

/* used to sort lines by segment and offset */
static int linesCompare(PLINEREF l1,PLINEREF l2)
{
	if(l1->section<l2->section) return -1;
	if(l1->section>l2->section) return +1;
	if(l1->ofs<l2->ofs) return -1;
	if(l1->ofs>l2->ofs) return +1;
	return 0;
}

static PSEG buildCodeViewInfo(PCHAR outname)
{
	PSEG debugData,dirList,subdirData,dataSeg,p,lineData;
	PDATABLOCK debugHeader,subdirEntry,data,symHeader,segHeader;
	UINT numSubdirs=0;
	UINT i,j,k,n,numLines;
	PCHAR name;
	UINT *sectionList=NULL;
	PCHAR *fileList=NULL;
	UINT numSections=0,numFiles=0,numFileSections;
	INT curseg;
	UINT minSegOfs,maxSegOfs;
	UINT *totalMinSegOfs=NULL,*totalMaxSegOfs=NULL;
	PREGION regions=NULL;
	UINT regionCount=0;

	debugData=createSection("CV Debug Data",NULL,NULL,NULL,0,1);
	debugData->internal=TRUE;
	debugData->use32=TRUE;

	debugHeader=createDataBlock(NULL,0,8+16,1);
	addData(debugData,debugHeader);
	debugHeader->data[0]='N';
	debugHeader->data[1]='B';
	debugHeader->data[2]='0';
	debugHeader->data[3]='9';
	debugHeader->data[4]=8; /* offset to directory */
	debugHeader->data[8]=16; /* length of directory header */
	debugHeader->data[10]=12; /* length of each entry */

	dirList=createSection("Subdirectory list",NULL,NULL,NULL,0,1);
	dirList->internal=TRUE;
	addSeg(debugData,dirList);

	subdirData=createSection("Subdirectory data",NULL,NULL,NULL,0,1);
	subdirData->internal=TRUE;
	addSeg(debugData,subdirData);

	/* add module subsection */
	subdirEntry=createDataBlock(NULL,0,12,4);
	addData(dirList,subdirEntry);
	numSubdirs++;

	Set16(&subdirEntry->data[PE_DEBUG_SUBDIR_TYPE],PE_DEBUG_SUB_MODULE);
	subdirEntry->data[PE_DEBUG_SUBDIR_MODULE]=1;

	dataSeg=createSection("module map",NULL,NULL,NULL,0,4);
	dataSeg->internal=TRUE;
	addSeg(subdirData,dataSeg);

	/* module header */
	symHeader=createDataBlock(NULL,0,8,4);
	addData(dataSeg,symHeader);

	symHeader->data[PE_DEBUG_MODULE_STYLE]='C';
	symHeader->data[PE_DEBUG_MODULE_STYLE+1]='V';

	/* add an entry for each output seg */
	for(i=0,j=0;i<globalSegCount;++i)
	{
		if(!globalSegs[i]) continue;
		if(globalSegs[i]->absolute) continue;
		/* empty segments don't go in object table */
		if(!globalSegs[i]->length && (globalSegs[i]!=debugSeg)) continue;

		/* OK, got a segment to add to list */
		j++;

		/* get line numbers for this segment */
		getLines(globalSegs[i],j,0);

		/* and add segment to module seg-list */
		data=createDataBlock(NULL,0,12,4);
		addData(dataSeg,data);

		Set32(&data->data[8],globalSegs[i]->length);


		/* add reloc to get segment number and seg offset */
		dataSeg->relocs=checkRealloc(dataSeg->relocs,(dataSeg->relocCount+2)*sizeof(RELOC));
		dataSeg->relocs[dataSeg->relocCount].base=REL_ABS;
		dataSeg->relocs[dataSeg->relocCount].rtype=REL_SEG;
		dataSeg->relocs[dataSeg->relocCount].tseg=globalSegs[i];
		dataSeg->relocs[dataSeg->relocCount].fseg=NULL;
		dataSeg->relocs[dataSeg->relocCount].text=NULL;
		dataSeg->relocs[dataSeg->relocCount].fext=NULL;
		dataSeg->relocs[dataSeg->relocCount].disp=0;
		dataSeg->relocs[dataSeg->relocCount].ofs=data->offset;
		dataSeg->relocCount++;
		/* get offset */
		dataSeg->relocs[dataSeg->relocCount].base=REL_FRAME;
		dataSeg->relocs[dataSeg->relocCount].rtype=REL_OFS32;
		dataSeg->relocs[dataSeg->relocCount].tseg=globalSegs[i];
		dataSeg->relocs[dataSeg->relocCount].fseg=globalSegs[i];
		dataSeg->relocs[dataSeg->relocCount].text=NULL;
		dataSeg->relocs[dataSeg->relocCount].fext=NULL;
		dataSeg->relocs[dataSeg->relocCount].disp=0;
		dataSeg->relocs[dataSeg->relocCount].ofs=data->offset+4;
		dataSeg->relocCount++;
	}

	symHeader->data[PE_DEBUG_MODULE_NUMSEGS]=j;

	/* add module name */
	name=outname;
	while(strpbrk(name,PATHCHARS))
	{
		name=strpbrk(name,PATHCHARS)+1;
	}

	data=createDataBlock(NULL,0,strlen(name)+1,1);
	addData(dataSeg,data);

	data->data[0]=strlen(name);
	memcpy(data->data+1,name,strlen(name));

	Set32(&subdirEntry->data[PE_DEBUG_SUBDIR_LENGTH],dataSeg->length);
	dirList->relocs=checkRealloc(dirList->relocs,(dirList->relocCount+1)*sizeof(RELOC));
	dirList->relocs[dirList->relocCount].base=REL_FRAME;
	dirList->relocs[dirList->relocCount].fseg=debugData;
	dirList->relocs[dirList->relocCount].tseg=dataSeg;
	dirList->relocs[dirList->relocCount].disp=0;
	dirList->relocs[dirList->relocCount].ofs=subdirEntry->offset+PE_DEBUG_SUBDIR_OFFSET;
	dirList->relocs[dirList->relocCount].rtype=REL_OFS32;
	dirList->relocs[dirList->relocCount].text=NULL;
	dirList->relocs[dirList->relocCount].fext=NULL;
	dirList->relocCount++;

	/* add line-numbers subsection */
	subdirEntry=createDataBlock(NULL,0,12,4);
	addData(dirList,subdirEntry);
	numSubdirs++;

	Set16(&subdirEntry->data[PE_DEBUG_SUBDIR_TYPE],PE_DEBUG_SUB_SRCMODULE);
	subdirEntry->data[PE_DEBUG_SUBDIR_MODULE]=1;

	dataSeg=createSection("line number map",NULL,NULL,NULL,0,4);
	dataSeg->internal=TRUE;
	addSeg(subdirData,dataSeg);

	/* sort line-numbers into seg/offset order */
	qsort(debugLines,debugLineCount,sizeof(LINEREF),(PCOMPAREFUNC)linesCompare);
	/* build regions, file list and section list */
	for(i=0;i<debugLineCount;++i)
	{
		if(!i || strcmp(debugLines[i-1].filename,debugLines[i].filename) ||
		   (debugLines[i-1].section!=debugLines[i].section))
		{
			regions=checkRealloc(regions,(regionCount+1)*sizeof(REGION));
			regions[regionCount].start=i;
			if(regionCount)
			{
				regions[regionCount-1].end=i-1;
			}
			regionCount++;
		}
		for(k=0;k<numFiles;++k)
		{
			if(!strcmp(fileList[k],debugLines[i].filename)) break;
		}
		if(k==numFiles)
		{
			fileList=checkRealloc(fileList,(numFiles+1)*sizeof(PCHAR));
			fileList[numFiles]=debugLines[i].filename;
			numFiles++;
		}

		for(k=0;k<numSections;++k)
		{
			if(sectionList[k]==debugLines[i].section) break;
		}
		if(k==numSections)
		{
			sectionList=checkRealloc(sectionList,(numSections+1)*sizeof(UINT));
			sectionList[numSections]=debugLines[i].section;
			numSections++;
		}
	}
	if(regionCount)
	{
		regions[regionCount-1].end=i-1;
	}

	/* allocate a header entry with sufficient size */
	symHeader=createDataBlock(NULL,0,4+4*numFiles+10*numSections,4);
	addData(dataSeg,symHeader);
	/* store count of files and sections */
	Set16(&symHeader->data[0],numFiles);
	Set16(&symHeader->data[2],numSections);

	/* fill in section reference list */
	for(k=4+4*numFiles+8*numSections,i=0;i<numSections;++i,k+=2)
	{
		Set16(&symHeader->data[k],sectionList[i]);
	}

	/* free section list data */
	totalMinSegOfs=checkMalloc(numSections*sizeof(UINT));
	totalMaxSegOfs=checkMalloc(numSections*sizeof(UINT));

	for(i=0;i<numSections;++i)
	{
		totalMinSegOfs[i]=UINT_MAX;
		totalMaxSegOfs[i]=0;
	}

	numFileSections=0;

	/* fill in data for each file in list */
	for(i=0;i<numFiles;++i)
	{
		name=fileList[i];

		lineData=createSection(name,NULL,NULL,NULL,0,4);
		lineData->internal=TRUE;
		addSeg(dataSeg,lineData);

		/* get offset of line data for this file */
		Set32(&symHeader->data[4+4*i],lineData->base);

		/* count number of regions in this file */
		numFileSections=0;
		for(j=0;j<regionCount;++j)
		{
			if(!strcmp(name,debugLines[regions[j].start].filename)) ++numFileSections;
		}

		/* build section table for this file */
		segHeader=createDataBlock(NULL,0,4+12*numFileSections+1+strlen(name),4);
		addData(lineData,segHeader);
		Set16(&segHeader->data[0],numFileSections);
		/* which finishes with the file name */
		segHeader->data[4+12*numFileSections]=strlen(name)&0xff;
		// segHeader->data[4+12*numFileSections+1]=(strlen(name)>>8)&0xff;
		memcpy(segHeader->data+4+12*numFileSections+1,name,strlen(name));

		/* now build the segment entries */
		for(k=0,j=0;k<numFileSections;++k,++j)
		{
			while(strcmp(debugLines[regions[j].start].filename,name)) ++j;

			curseg=debugLines[regions[j].start].section;
			numLines=regions[j].end-regions[j].start+1;

			data=createDataBlock(NULL,0,4+6*numLines,4);
			addData(lineData,data);

			/* get offset of line data for this section */
			Set32(&segHeader->data[4+4*k],lineData->base+data->offset);

			Set16(&data->data[0],curseg);
			Set16(&data->data[2],numLines);

			minSegOfs=UINT_MAX;
			maxSegOfs=0;

			/* fill in line number info for this region */
			for(n=0;n<numLines;++n)
			{
				if(debugLines[regions[j].start+n].ofs<minSegOfs) minSegOfs=debugLines[regions[j].start+n].ofs;
				if(debugLines[regions[j].start+n].ofs>maxSegOfs) maxSegOfs=debugLines[regions[j].start+n].ofs;

				Set32(&data->data[4+4*n],debugLines[regions[j].start+n].ofs);
				Set16(&data->data[4+4*numLines+2*n],debugLines[regions[j].start+n].num);
			}
			/* if a valid extent, fill that in too */
			if(minSegOfs<=maxSegOfs)
			{
				Set32(&segHeader->data[4+4*numFileSections+8*k],minSegOfs);
				Set32(&segHeader->data[4+4*numFileSections+8*k+4],maxSegOfs);
				/* extend size of encompassing section too */
				for(n=0;n<numSections;++n)
				{
					if(sectionList[n]==curseg) break;
				}
				if(minSegOfs<totalMinSegOfs[n]) totalMinSegOfs[n]=minSegOfs;
				if(maxSegOfs>totalMaxSegOfs[n]) totalMaxSegOfs[n]=maxSegOfs;
			}
		}
	}

	/* fill in calculated sizes of sections */
	for(k=0;k<numSections;++k)
	{
		if(totalMinSegOfs[k]<=totalMaxSegOfs[k])
		{
			Set32(&symHeader->data[4+4*numFiles+8*k],totalMinSegOfs[k]);
			Set32(&symHeader->data[4+4*numFiles+8*k+4],totalMaxSegOfs[k]);
		}
	}

	/* tidy up subdir entry */

	Set32(&subdirEntry->data[PE_DEBUG_SUBDIR_LENGTH],dataSeg->length);
	dirList->relocs=checkRealloc(dirList->relocs,(dirList->relocCount+1)*sizeof(RELOC));
	dirList->relocs[dirList->relocCount].base=REL_FRAME;
	dirList->relocs[dirList->relocCount].fseg=debugData;
	dirList->relocs[dirList->relocCount].tseg=dataSeg;
	dirList->relocs[dirList->relocCount].disp=0;
	dirList->relocs[dirList->relocCount].ofs=subdirEntry->offset+PE_DEBUG_SUBDIR_OFFSET;
	dirList->relocs[dirList->relocCount].rtype=REL_OFS32;
	dirList->relocs[dirList->relocCount].text=NULL;
	dirList->relocs[dirList->relocCount].fext=NULL;
	dirList->relocCount++;

	/* add symbols subsection */
	subdirEntry=createDataBlock(NULL,0,12,4);
	addData(dirList,subdirEntry);
	numSubdirs++;

	Set16(&subdirEntry->data[PE_DEBUG_SUBDIR_TYPE],PE_DEBUG_SUB_GLOBALPUB);
	Set16(&subdirEntry->data[PE_DEBUG_SUBDIR_MODULE],0xffff);


	dataSeg=createSection("symbol Data",NULL,NULL,NULL,0,4);
	dataSeg->internal=TRUE;
	addSeg(subdirData,dataSeg);

	symHeader=createDataBlock(NULL,0,16,4);
	addData(dataSeg,symHeader);

	for(i=0;i<globalSymbolCount;++i)
	{
		if(!globalSymbols[i]) continue;
		if(globalSymbols[i]->type==PUB_LIBSYM) continue;

		/* get length of memory block */
		j=13+strlen(globalSymbols[i]->name);

		/* align to 4-byte boundary */
		j+=3;
		j&=0xfffffffc;

		data=createDataBlock(NULL,0,j,4);
		addData(dataSeg,data);

		/* store length of block */
		Set16(data->data,(j-2));
		/* public symbol */
		data->data[2]=3;
		data->data[3]=2;
		/* name */
		data->data[12]=strlen(globalSymbols[i]->name);
		memcpy(data->data+13,globalSymbols[i]->name,strlen(globalSymbols[i]->name));

		/* add relocs to get segment number and offset */
		dataSeg->relocs=checkRealloc(dataSeg->relocs,(dataSeg->relocCount+2)*sizeof(RELOC));
		dataSeg->relocs[dataSeg->relocCount].base=REL_ABS;
		dataSeg->relocs[dataSeg->relocCount].rtype=REL_SEG;
		dataSeg->relocs[dataSeg->relocCount].tseg=globalSymbols[i]->seg;
		dataSeg->relocs[dataSeg->relocCount].fseg=NULL;
		dataSeg->relocs[dataSeg->relocCount].text=NULL;
		dataSeg->relocs[dataSeg->relocCount].fext=NULL;
		dataSeg->relocs[dataSeg->relocCount].disp=0;
		dataSeg->relocs[dataSeg->relocCount].ofs=data->offset+8;
		dataSeg->relocCount++;
		/* done seg, now do offset */
		p=globalSymbols[i]->seg;
		while(p->parent && (p->section<0))
		{
			p=p->parent;
		}

		dataSeg->relocs[dataSeg->relocCount].base=REL_FRAME;
		dataSeg->relocs[dataSeg->relocCount].rtype=REL_OFS32;
		dataSeg->relocs[dataSeg->relocCount].tseg=globalSymbols[i]->seg;
		dataSeg->relocs[dataSeg->relocCount].fseg=p;
		dataSeg->relocs[dataSeg->relocCount].text=NULL;
		dataSeg->relocs[dataSeg->relocCount].fext=NULL;
		dataSeg->relocs[dataSeg->relocCount].disp=globalSymbols[i]->ofs;
		dataSeg->relocs[dataSeg->relocCount].ofs=data->offset+4;
		dataSeg->relocCount++;
	}

	Set32(&symHeader->data[4],(dataSeg->length-16));


	Set32(&subdirEntry->data[PE_DEBUG_SUBDIR_LENGTH],dataSeg->length);

	dirList->relocs=checkRealloc(dirList->relocs,(dirList->relocCount+1)*sizeof(RELOC));
	dirList->relocs[dirList->relocCount].base=REL_FRAME;
	dirList->relocs[dirList->relocCount].fseg=debugData;
	dirList->relocs[dirList->relocCount].tseg=dataSeg;
	dirList->relocs[dirList->relocCount].disp=0;
	dirList->relocs[dirList->relocCount].ofs=subdirEntry->offset+PE_DEBUG_SUBDIR_OFFSET;
	dirList->relocs[dirList->relocCount].rtype=REL_OFS32;
	dirList->relocs[dirList->relocCount].text=NULL;
	dirList->relocs[dirList->relocCount].fext=NULL;
	dirList->relocCount++;

	/* add entry for file index */
	if(numFiles)
	{
		subdirEntry=createDataBlock(NULL,0,12,4);
		addData(dirList,subdirEntry);
		numSubdirs++;

		Set16(&subdirEntry->data[PE_DEBUG_SUBDIR_TYPE],PE_DEBUG_SUB_FILEINDEX);
		Set16(&subdirEntry->data[PE_DEBUG_SUBDIR_MODULE],0xffff);

		dataSeg=createSection("File Index",NULL,NULL,NULL,0,4);
		dataSeg->internal=TRUE;
		addSeg(subdirData,dataSeg);

		symHeader=createDataBlock(NULL,0,8+4*numFiles,4);
		addData(dataSeg,symHeader);

		symHeader->data[0]=1;
		Set16(&symHeader->data[2],numFiles);
		Set16(&symHeader->data[6],numFiles);

		for(i=0;i<numFiles;++i)
		{
			name=fileList[i];
			data=createDataBlock(name,0,strlen(name)+1,1);
			addData(dataSeg,data);
			Set32(&symHeader->data[8+i*4],(data->offset-(8+4*numFiles)));
		}

		/* tidy up */
		Set32(&subdirEntry->data[PE_DEBUG_SUBDIR_LENGTH],dataSeg->length);

		dirList->relocs=checkRealloc(dirList->relocs,(dirList->relocCount+1)*sizeof(RELOC));
		dirList->relocs[dirList->relocCount].base=REL_FRAME;
		dirList->relocs[dirList->relocCount].fseg=debugData;
		dirList->relocs[dirList->relocCount].tseg=dataSeg;
		dirList->relocs[dirList->relocCount].disp=0;
		dirList->relocs[dirList->relocCount].ofs=subdirEntry->offset+PE_DEBUG_SUBDIR_OFFSET;
		dirList->relocs[dirList->relocCount].rtype=REL_OFS32;
		dirList->relocs[dirList->relocCount].text=NULL;
		dirList->relocs[dirList->relocCount].fext=NULL;
		dirList->relocCount++;
	}

	/* add entry for segment map */
	subdirEntry=createDataBlock(NULL,0,12,4);
	addData(dirList,subdirEntry);
	numSubdirs++;

	Set16(&subdirEntry->data[PE_DEBUG_SUBDIR_TYPE],PE_DEBUG_SUB_SEGMAP);
	Set16(&subdirEntry->data[PE_DEBUG_SUBDIR_MODULE],0xffff);


	dataSeg=createSection("Segment map",NULL,NULL,NULL,0,4);
	dataSeg->internal=TRUE;
	addSeg(subdirData,dataSeg);

	symHeader=createDataBlock(NULL,0,4,4);
	addData(dataSeg,symHeader);

	/* add an entry for each output seg */
	for(i=0,j=0;i<globalSegCount;++i)
	{
		if(!globalSegs[i]) continue;
		if(globalSegs[i]->absolute) continue;
		/* empty segments don't go in object table */
		if(!globalSegs[i]->length && (globalSegs[i]!=debugSeg)) continue;
		j++;
		data=createDataBlock(NULL,0,20,4);
		addData(dataSeg,data);

		/* store flags */
		data->data[0]=8 | (globalSegs[i]->execute?4:0) | (globalSegs[i]->write?2:0)| (globalSegs[i]->read?1:0);

		/* store seg+class name indices */
		data->data[8]=0xff;
		data->data[9]=0xff;
		data->data[10]=0xff;
		data->data[11]=0xff;


		/* store section number */
		Set16(&data->data[6],j);


		/* store length */
		Set32(&data->data[16],globalSegs[i]->length);


		/* add reloc to get segment number and seg offset */
		dataSeg->relocs=checkRealloc(dataSeg->relocs,(dataSeg->relocCount+2)*sizeof(RELOC));
		dataSeg->relocs[dataSeg->relocCount].base=REL_ABS;
		dataSeg->relocs[dataSeg->relocCount].rtype=REL_SEG;
		dataSeg->relocs[dataSeg->relocCount].tseg=globalSegs[i];
		dataSeg->relocs[dataSeg->relocCount].fseg=NULL;
		dataSeg->relocs[dataSeg->relocCount].text=NULL;
		dataSeg->relocs[dataSeg->relocCount].fext=NULL;
		dataSeg->relocs[dataSeg->relocCount].disp=0;
		dataSeg->relocs[dataSeg->relocCount].ofs=data->offset;
		dataSeg->relocCount++;
		/* get offset */
		dataSeg->relocs[dataSeg->relocCount].base=REL_FRAME;
		dataSeg->relocs[dataSeg->relocCount].rtype=REL_OFS32;
		dataSeg->relocs[dataSeg->relocCount].tseg=globalSegs[i];
		dataSeg->relocs[dataSeg->relocCount].fseg=globalSegs[i];
		dataSeg->relocs[dataSeg->relocCount].text=NULL;
		dataSeg->relocs[dataSeg->relocCount].fext=NULL;
		dataSeg->relocs[dataSeg->relocCount].disp=0;
		dataSeg->relocs[dataSeg->relocCount].ofs=data->offset+4;
		dataSeg->relocCount++;
	}

	/* store number of segments in map */
	Set16(&symHeader->data[0],j);
	Set16(&symHeader->data[2],j);

	Set32(&subdirEntry->data[PE_DEBUG_SUBDIR_LENGTH],dataSeg->length);

	dirList->relocs=checkRealloc(dirList->relocs,(dirList->relocCount+1)*sizeof(RELOC));
	dirList->relocs[dirList->relocCount].base=REL_FRAME;
	dirList->relocs[dirList->relocCount].fseg=debugData;
	dirList->relocs[dirList->relocCount].tseg=dataSeg;
	dirList->relocs[dirList->relocCount].disp=0;
	dirList->relocs[dirList->relocCount].ofs=subdirEntry->offset+PE_DEBUG_SUBDIR_OFFSET;
	dirList->relocs[dirList->relocCount].rtype=REL_OFS32;
	dirList->relocs[dirList->relocCount].text=NULL;
	dirList->relocs[dirList->relocCount].fext=NULL;
	dirList->relocCount++;

	/* fill in "number of subdir entries" field */
	debugHeader->data[12]=numSubdirs;


	checkFree(regions);
	checkFree(fileList);
	checkFree(sectionList);
	checkFree(totalMinSegOfs);
	checkFree(totalMaxSegOfs);

	return debugData;
}


static BOOL buildPEDebug(PCHAR name)
{
	PSEG debugData;
	PDATABLOCK dirEntry;
	time_t now;

	/* debug section starts with a directory listing the types of debug info available */
	debugDir=createSection("Debug Directory",NULL,NULL,NULL,0,1);
	debugDir->internal=TRUE;
	debugDir->use32=TRUE;
	addSeg(debugSeg,debugDir);

	/* we want to create CodeView debug info */
	debugData=buildCodeViewInfo(name);
	if(debugData)
	{
		/* add it to debug info seg */
		addSeg(debugSeg,debugData);

		/* we need a directory entry for it */
		dirEntry=createDataBlock(NULL,0,PE_DEBUGDIR_SIZE,1);
		addData(debugDir,dirEntry);
		time(&now);
		Set32(&dirEntry->data[4],now);

		dirEntry->data[12]=PE_DEBUG_CODEVIEW;

		/* add length entry */
		Set32(&dirEntry->data[16],debugData->length);

		/* add "pointer to real data" relocs to directory entry */
		debugDir->relocs=checkRealloc(debugDir->relocs,(debugDir->relocCount+2)*sizeof(RELOC));
		debugDir->relocs[debugDir->relocCount].ofs=dirEntry->offset+20;
		debugDir->relocs[debugDir->relocCount].disp=0;
		debugDir->relocs[debugDir->relocCount].tseg=debugData;
		debugDir->relocs[debugDir->relocCount].fseg=NULL;
		debugDir->relocs[debugDir->relocCount].fext=debugDir->relocs[debugDir->relocCount].text=NULL;
		debugDir->relocs[debugDir->relocCount].base=REL_RVA;
		debugDir->relocs[debugDir->relocCount].rtype=REL_OFS32;
		debugDir->relocCount++;
		/* we've done RVA, now do file offset */
		debugDir->relocs[debugDir->relocCount].ofs=dirEntry->offset+24;
		debugDir->relocs[debugDir->relocCount].disp=0;
		debugDir->relocs[debugDir->relocCount].tseg=debugData;
		debugDir->relocs[debugDir->relocCount].fseg=NULL;
		debugDir->relocs[debugDir->relocCount].fext=debugDir->relocs[debugDir->relocCount].text=NULL;
		debugDir->relocs[debugDir->relocCount].base=REL_FILEPOS;
		debugDir->relocs[debugDir->relocCount].rtype=REL_OFS32;
		debugDir->relocCount++;
	}

	return TRUE;
}

static UINT calcSegChecksum(PSEG s,UINT *pos)
{
	UINT i,j;
	PDATABLOCK d;
	UINT sum=0;

	if(!s) return 0;
	if(!pos) return 0;

	i=*pos;
	if(i>s->filepos)
	{
		addError("Segment overlap in file\n");
		return FALSE;
	}

	for(i=0;i<s->contentCount;i++)
	{
		switch(s->contentList[i].flag)
		{
		case DATA:
			d=s->contentList[i].data;
			/* no output for zero-length data blocks */
			if(!d->length) break;
			j=s->filepos+d->offset-(*pos); /* get number of zeroes to pad with */
			while(j)
			{
				(*pos)++;
				j--;
			}
			for(j=0;j<d->length;++j,++(*pos))
			{
				if((*pos)&1)
				{
					sum+=d->data[j]<<8;
				}
				else
				{
					sum+=d->data[j];
				}
				sum=(sum&0xffff)+(sum>>16);
			}

			break;
		case SEGMENT:
			if(!s->contentList[i].seg->absolute)
			{
				if(!s->contentList[i].seg->fpset)
					s->contentList[i].seg->filepos=s->filepos+s->contentList[i].seg->base;
				sum+=calcSegChecksum(s->contentList[i].seg,pos);
				sum=(sum&0xffff)+(sum>>16);
				sum=(sum&0xffff)+(sum>>16);
			}

			break;
		}
	}
	return sum;
}

BOOL PEFinalise(PCHAR name)
{
	PSEG h,a,header;
	PDATABLOCK stubBlock;
	UINT i,fp;

	a=createSection("Global",NULL,NULL,NULL,0,1);
	a->internal=TRUE;
	a->addressspace=TRUE;
	a->use32=TRUE;
	a->base=imageBase;
	h=createSection("header",NULL,NULL,NULL,0,1);
	h->internal=TRUE;
	h->use32=TRUE;
	addSeg(a,h);

	stubBlock=createDataBlock(stub,0,stubSize,1);
	addData(h,stubBlock);
	if(stub!=defaultStub) checkFree(stub);

	buildPEImports();
	buildPEResources();
	buildPEExports(name);
	if(relocsRequired)
	{
		buildPERelocs();
	}
	if(importSeg && !importSeg->length && !importSeg->parent)
	{
		freeSection(importSeg);
		importSeg=NULL;
		globalSegs[impSegNum]=NULL;
	}
	if(exportSeg && !exportSeg->length && !exportSeg->parent)
	{
		freeSection(exportSeg);
		exportSeg=NULL;
		globalSegs[expSegNum]=NULL;
	}
	if(relocSeg && !relocSeg->length && !relocSeg->parent)
	{
		freeSection(relocSeg);
		relocSeg=NULL;
		globalSegs[relSegNum]=NULL;
	}
	if(resourceSeg && !resourceSeg->length && !resourceSeg->parent)
	{
		freeSection(resourceSeg);
		resourceSeg=NULL;
		globalSegs[resSegNum]=NULL;
	}

	if(debugRequired)
	{
		buildPEDebug(name);
	}
	if(debugSeg && !debugSeg->length && !debugSeg->parent)
	{
		freeSection(debugSeg);
		debugSeg=NULL;
		globalSegs[debSegNum]=NULL;
	}

	addSeg(h,header=createPEHeader());

	Set32(&stubBlock->data[PE_SIGNATURE_OFFSET],header->base);

	spaceList=checkMalloc(sizeof(PSEG));
	spaceList[0]=a;
	spaceCount=1;

	fp=a->length;

	for(i=0;i<globalSegCount;++i)
	{
		if(!globalSegs[i]) continue;
		if(!globalSegs[i]->absolute)
		{
			fp+=fileAlign-1;
			fp&=0xffffffff-(fileAlign-1);
			globalSegs[i]->filepos=fp;
			globalSegs[i]->fpset=TRUE;
			fp+=getInitLength(globalSegs[i]);
		}

		addSeg(a,globalSegs[i]);
		globalSegs[i]=NULL;
	}

	performFixups(a);

	fp=0;
	i=calcSegChecksum(a,&fp);
	i+=fp;
	Set32(&header->contentList[0].data->data[PE_CHECKSUM],i);

	return TRUE;
}
