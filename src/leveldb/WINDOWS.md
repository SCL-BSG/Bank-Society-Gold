# Building LevelDB On Windows

## Prereqs 

Install the [Windows Software Development Kit version 7.1](http://www.microsoft.com/downloads/dlx/en-us/listdetailsview.aspx?FamilyID=6b6c21d2-2006-4afa-9702-529fa782d63b).

Download and extract the [Snappy source distribution](http://snappy.googlecode.com/files/snappy-1.0.5.tar.gz)

1. Open the "Windows SDK 7.1 Command Prompt" :
   Start Menu -> "Microsoft Windows SDK v7.1" > "Windows SDK 7.1 Command Prompt"
2. Change the directory to the leveldb project

## Building the Static lib 

* 32 bit Version 

        setenv /x86
        msbuild.exe /p:Configuration=Release /p:Platform=Win32 /p:Snappy=..\snappy-1.0.5

* 64 bit Version 

        setenv /x64
        msbuild.exe /p:Configuration=Release /p:Platform=x64 /p:Snappy=..\snappy-1.0.5


## Building and Running the Benchmark app

* 32 bit Version 

	    setenv /x86
	    msbuild.exe /p:Configuration=Benchmark /p:Platform=Win32 /p:Snappy=..\snappy-1.0.5
		Benchmark\leveldb.exe

* 64 bit Version 

	    setenv /x64
	    msbuild.exe /p:Configuration=Benchmark /p:Platform=x64 /p:Snappy=..\snappy-1.0.5
	    x64\Benchmark\leveldb.exe




#-I"d:/project/society/include/boost-1_57" \
 
# NOTE  _ LEVELDB needed to be built
#If you are native on Windows, you should just comment out from #https://github.com/bitcoin/bitcoin/blob/master/bitcoin-qt.pro#L97 up to line 111, as this currently doesn't #work on Windows.

#I guess you have MinGW installed, so you are able to compile LevelDB via MinGW Shell.

#cd /c/Users/Diapolo/bitcoin.Qt/src/leveldb (use your path to leveldb dir)
#TARGET_OS=OS_WINDOWS_CROSSCOMPILE make libleveldb.a libmemenv.a

#I also needed to edit the src\leveldb\build_detect_platform file to include my used Boost version (1.51 in #my case), which changes line 119 into PLATFORM_EXTRALIBS="-lboost_system-mgw47-mt-s-1_51 -#lboost_filesystem-mgw47-mt-s-1_51 -lboost_thread-mgw47-mt-s-1_51 -lboost_chrono-mgw47-mt-s-1_51"

#Edit: Pull #1940 is related to this, perhaps you want to take a look there, too.

