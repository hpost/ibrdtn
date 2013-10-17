/*
 * dtnoutbox.cpp
 *
 * Copyright (C) 2013 IBR, TU Braunschweig
 *
 * Written-by: Johannes Morgenroth <morgenroth@ibr.cs.tu-bs.de>
 * 			   David Goltzsche <goltzsch@ibr.cs.tu-bs.de>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "config.h"
#include <ibrdtn/api/Client.h>
#include <ibrcommon/net/socket.h>
#include <ibrcommon/thread/Mutex.h>
#include <ibrcommon/thread/MutexLock.h>
#include <ibrdtn/data/PayloadBlock.h>
#include <ibrcommon/data/BLOB.h>
#include <ibrcommon/data/File.h>
#include <ibrcommon/appstreambuf.h>

#include "io/TarUtils.h"
#include "io/ObservedNormalFile.h"
#include "io/FileHashList.h"

#ifdef HAVE_LIBTFFS
#include "io/ObservedFATFile.h"
extern "C"
{
#include <tffs.h>
}
#endif

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#endif

#include <stdlib.h>
#include <iostream>
#include <map>
#include <vector>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#include <getopt.h>
using namespace ibrcommon;

// set this variable to false to stop the app
bool _running = true;

// global connection
ibrcommon::socketstream *_conn = NULL;

//global wait conditional
ibrcommon::Conditional _wait_cond;
bool _wait_abort = false;

//global conf values
string _conf_name;
string _conf_outbox;
string _conf_destination;

//optional paramters
string _conf_workdir;
size_t _conf_interval = 5000;
size_t _conf_rounds = 3;
string _conf_path = "/";
int _conf_quiet = false;
int _conf_fat = false;

void print_help()
{
	cout << "-- dtnoutbox (IBR-DTN) --" << endl;
	cout << "Syntax: dtnoutbox [options] <name> <outbox> <destination>" << endl;
	cout << " <name>                     the application name" << endl;
#ifdef HAVE_LIBTFFS
	cout << " <outbox>                   location of outgoing files, directory or vfat-image" << endl;
#else
	cout << " <outbox>                   directory of outgoing files" << endl;
#endif
	cout << " <destination>              the destination EID for all outgoing files" << endl << endl;
	cout << "* optional parameters *" << endl;
	cout << " -h|--help                  display this text" << endl;
	cout << " -w|--workdir <dir>         temporary work directory" << endl;
	cout << " -i|--interval <interval>   interval in milliseconds, in which <outbox> is scanned for new/changed files. default: 5000" << endl;
	cout << " -r|--rounds <number>       number of rounds of intervals, after which a unchanged file is considered as written. default: 3" << endl;
#ifdef HAVE_LIBTFFS
	cout << " -p|--path <path>           path of outbox within vfat-image. default: /" << endl;
#endif
	cout << " -q|--quiet                 only print error messages" << endl;

	_running = false; //stop this app, after printing help

}

struct option long_options[] =
{
    {"name",required_argument,0, 'n'},
    {"outbox",required_argument,0, 'o'},
    {"destination",  required_argument, 0, 'd'},
    {"help", no_argument, 0, 'h'},
    {"workdir", required_argument, 0, 'w'},
    {"interval", required_argument, 0, 'i'},
    {"rounds", required_argument, 0, 'r'},
    {"path", required_argument, 0, 'p'},
    {"regex", required_argument, 0, 'R'},
    {"quiet", no_argument, 0, 'q'},
    {0, 0, 0, 0}
};
void read_configuration(int argc, char** argv)
{
	// print help if not enough parameters are set
	if (argc < 4)
	{
		print_help();
		exit(0);
	}

	while(1)
	{
		/* getopt_long stores the option index here. */
		int option_index = 0;
		int c = getopt_long (argc, argv, "hw:i:r:p:R:q",
				long_options, &option_index);
		/* Detect the end of the options. */
		if (c == -1)
			break;

		switch (c)
		{
		case 0:
			/* If this option set a flag, do nothing else now. */
			if (long_options[option_index].flag != 0)
				break;
			printf ("option %s", long_options[option_index].name);
			if (optarg)
				printf (" with arg %s", optarg);
			printf ("\n");
			break;

		case 'h':
			print_help();
			break;
		case 'w':
			_conf_workdir = std::string(optarg);
			break;
		case 'i':
			_conf_interval = atoi(optarg);
			break;
		case 'r':
			_conf_rounds = atoi(optarg);
			break;
		case 'p':
			_conf_path = std::string(optarg);
			break;
		case 'q':
			_conf_quiet = true;
			break;
		case '?':
			break;
		default:
			printf("error: %c\n",c);
		}
	}
	// print help if there are not enough or too many remaining parameters
	if (argc - optind != 3)
	{
		print_help();
		exit(0);
	}

	_conf_name = std::string(argv[optind]);
	_conf_outbox = std::string(argv[optind+1]);
	_conf_destination = std::string(argv[optind+2]);
}
void sighandler(int signal)
{
	switch (signal)
	{
	case SIGTERM:
	case SIGINT:
	{
		//stop waiting and stop running, on SIGINT or SIGTERM
		ibrcommon::MutexLock l(_wait_cond);
        _running = false;
		_wait_cond.signal(true);
        if (_conn != NULL) _conn->close();
        break;
	}
#ifndef __WIN32__
	case SIGUSR1:
	{
		//stop waiting on SIGUSR1 -> "quickscan"
		ibrcommon::MutexLock l(_wait_cond);
		_wait_abort = true;
		_wait_cond.signal(true);
		break;
	}
#endif
	default:
		break;
	}
}

bool deleteAll( ObservedFile* ptr){
	delete ptr;
	return true;
}
/*
 * main application method
 */
int main( int argc, char** argv )
{
	// catch process signals
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
#ifndef __WIN32__
	signal(SIGUSR1, sighandler);
	signal(SIGUSR2, sighandler);
#endif


	// read the configuration
	read_configuration(argc,argv);
	// init working directory
	if (_conf_workdir.length() > 0)
	{
		ibrcommon::File blob_path(_conf_workdir);

		if (blob_path.exists())
		{
			ibrcommon::BLOB::changeProvider(new ibrcommon::FileBLOBProvider(blob_path), true);
		}
	}

	// backoff for reconnect
	unsigned int backoff = 2;

	File outbox_file(_conf_outbox);

	std::list<ObservedFile*> avail_files, new_files, old_files, deleted_files, observed_files, files_to_send;
	std::list<ObservedFile*>::iterator iter;
	std::list<ObservedFile*>::iterator iter2;
	FileHashList sent_hashes;

	ObservedFile* outbox;

	ObservedFile::setConfigRounds(_conf_rounds);

	if(!outbox_file.getType() == DT_DIR)
	{
#ifdef HAVE_LIBTFFS
		_conf_fat = true;
		if(!outbox_file.exists())
		{
			cout << "ERROR: image not found! please create image before starting dtnoutbox" << endl;
			return -1;
		}

		ObservedFile::setConfigImgPath(_conf_outbox);
		outbox = new ObservedFATFile(_conf_path);
#else
		cout << "ERROR: image-file provided, but dtnoutbox has been compiled without libtffs support!" << endl;
		return -1;
#endif
	}
	else
	{
		if(!outbox_file.exists())
			File::createDirectory(outbox_file);

		outbox = new ObservedNormalFile(_conf_outbox);
	}

	// loop, if no stop if requested
	while (_running)
	{
		try
		{
			// Create a stream to the server using TCP.
			ibrcommon::vaddress addr("localhost", 4550);
			ibrcommon::socketstream conn(new ibrcommon::tcpsocket(addr));

			// set the connection globally
			_conn = &conn;

			// Initiate a client for synchronous receiving
			dtn::api::Client client(_conf_name, conn, dtn::api::Client::MODE_SENDONLY);

			// Connect to the server. Actually, this function initiate the
			// stream protocol by starting the thread and sending the contact header.
			client.connect();

			// reset backoff if connected
			backoff = 2;

			// check the connection
			while (_running)
			{

				deleted_files.clear();
				new_files.clear();
				old_files.clear();

				//store old files
				old_files = avail_files;
				avail_files.clear();

				//get all files
				outbox->getFiles(avail_files);

				//determine deleted files
				std::set_difference(old_files.begin(),old_files.end(),avail_files.begin(),avail_files.end(),std::back_inserter(deleted_files),ObservedFile::namecompare);
				//remove deleted files from observation
				for (iter = deleted_files.begin(); iter != deleted_files.end(); ++iter)
				{
					sent_hashes.removeAll((*iter)->getHash());
					for(iter2 = observed_files.begin();iter2 != observed_files.end();++iter2)
					{
							if((*iter2)->getHash() == (*iter)->getHash())
							{
								delete (*iter);
								observed_files.erase(iter2);
								break;
							}
					}
				}
				//rerun loop, if files have been deleted
				if(deleted_files.size() > 0)
					break;

				//determine new files
				std::set_difference(avail_files.begin(),avail_files.end(),old_files.begin(),old_files.end(),std::back_inserter(new_files),ObservedFile::namecompare);

				//add new files to observation
				for (iter = new_files.begin(); iter != new_files.end(); ++iter)
				{
					//TODO regexp auswerten

					ObservedFile* of;
					if(!_conf_fat)
						of = new ObservedNormalFile((*iter)->getPath());
#ifdef HAVE_LIBTFFS
					else
						of = new ObservedFATFile((*iter)->getPath());
#endif
					observed_files.push_back(of);
				}


				//tick and update all files
				for (iter = observed_files.begin(); iter != observed_files.end(); ++iter)
				{
					(*iter)->update();
					(*iter)->tick();
				}

				//find files to send, create std::list
				stringstream files_to_send_ss;
				files_to_send.clear();
				for (iter = observed_files.begin(); iter != observed_files.end(); ++iter)
				{
					if(!sent_hashes.contains((*iter)))
					{
						if ((*iter)->lastHashesEqual(_conf_rounds))
						{
							(*iter)->send();
							string hash = (*iter)->getHash();
							sent_hashes.add(FileHash(*iter));
							files_to_send_ss << (*iter)->getBasename() << " ";
							files_to_send.push_back(*iter);
						}
					}
				}

				if(!_conf_quiet)
					cout << files_to_send.size() << "/" << observed_files.size() << " files to send: " << files_to_send_ss.str() << endl;

				if (files_to_send.size())
				{
					// create a blob
					ibrcommon::BLOB::Reference blob = ibrcommon::BLOB::create();

//use libarchive...
#ifdef HAVE_LIBARCHIVE
	//if libarchive is available, check if libtffs can be used
					if(_conf_fat)
					{
						TarUtils::set_img_path(_conf_outbox);
						TarUtils::set_outbox_path(_conf_path);
					}
					else
						TarUtils::set_outbox_path(_conf_outbox);

					TarUtils::write_tar_archive(&blob, files_to_send);
//or commandline fallback
#else
					// "--remove-files" deletes files after adding
					//depending on configuration, this option is passed to tar or not
					std::string remove_string = " --remove-files";
					if(_conf_keep)
						remove_string = "";
					stringstream cmd;
					cmd << "tar" << remove_string << " -cO -C " << outbox.getPath() << " " << files_to_send_ss.str();

					// make a tar archive
					appstreambuf app(cmd.str(), appstreambuf::MODE_READ);
					istream stream(&app);

					// stream the content of "tar" to the payload block
					(*blob.iostream()) << stream.rdbuf();
#endif
					// create a new bundle
					dtn::data::EID destination = EID(_conf_destination);

					// create a new bundle
					dtn::data::Bundle b;

					// set destination
					b.destination = destination;

					// add payload block using the blob
					b.push_back(blob);

					// send the bundle
					client << b;
					client.flush();
				}

				if (_running)
				{
					// wait defined seconds
					ibrcommon::MutexLock l(_wait_cond);
					_wait_cond.wait(_conf_interval);
					if(_wait_abort)
						_wait_abort = false;
				}
			}

			observed_files.remove_if(deleteAll);

			// close the client connection
			client.close();

			// close the connection
			conn.close();

			// set the global connection to NULL
			_conn = NULL;
		} catch (const ibrcommon::socket_exception&)
		{
			// set the global connection to NULL
			_conn = NULL;

			if (_running)
			{
				cout << "Connection to bundle daemon failed. Retry in " << backoff << " seconds." << endl;
				ibrcommon::Thread::sleep(backoff * 1000);

				// if backoff < 10 minutes
				if (backoff < 600)
				{
					// set a new backoff
					backoff = backoff * 2;
				}
			}
		} catch (const ibrcommon::IOException&)
		{
			// set the global connection to NULL
			_conn = NULL;

			if (_running)
			{
				cout << "Connection to bundle daemon failed. Retry in " << backoff << " seconds." << endl;
				ibrcommon::Thread::sleep(backoff * 1000);

				// if backoff < 10 minutes
				if (backoff < 600)
				{
					// set a new backoff
					backoff = backoff * 2;
				}
			}
		} catch (const std::exception&)
		{
			// set the global connection to NULL
			_conn = NULL;
		}
	}

	return (EXIT_SUCCESS);
}
