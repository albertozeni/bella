#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <istream>
#include <vector>
#include <string>
#include <stdlib.h>
#include <algorithm>
#include <utility>
#include <array>
#include <tuple>
#include <queue>
#include <stack>
#include <functional>
#include <cstring>
#include <string.h>
#include <cassert>
#include <ios>
#include <unordered_map>
#include <sys/stat.h>
#include <map>
#include <omp.h>
#include "kmercode/hash_funcs.h"
#include "kmercode/Kmer.hpp"
#include "kmercode/Buffer.h"
#include "kmercode/common.h"
#include "kmercode/fq_reader.h"
#include "kmercode/ParallelFASTQ.h"

#include "mtspgemm2017/utility.h"
#include "mtspgemm2017/CSC.h"
#include "mtspgemm2017/CSR.h"
#include "mtspgemm2017/IO.h"
#include "mtspgemm2017/multiply.h"

#define LSIZE 16000
#define KMER_LENGTH 17
#define ITERS 10

using namespace std;

struct filedata {

    char filename[MAX_FILE_PATH];
    size_t filesize;
};

std::vector<filedata> GetFiles(char *filename) {
    int64_t totalsize = 0;
    int numfiles = 0;
    std::vector<filedata> filesview;
    
    filedata fdata;
    ifstream allfiles(filename);
    if(!allfiles.is_open()) {
        cerr << "could not open " << filename << endl;
        exit(1);
    }
    allfiles.getline(fdata.filename,MAX_FILE_PATH);
    while(!allfiles.eof())
    {
        struct stat st;
        stat(fdata.filename, &st);
        fdata.filesize = st.st_size;
        
        filesview.push_back(fdata);
        cout << filesview.back().filename << " : " << filesview.back().filesize / (1024*1024) << " MB" << endl;
        allfiles.getline(fdata.filename,MAX_FILE_PATH);
        totalsize += fdata.filesize;
        numfiles++;
    }
    return filesview;
}

typedef std::map<Kmer,size_t> dictionary; // <k-mer && reverse-complement, #kmers>
typedef std::vector<Kmer> Kmers;
typedef std::pair<size_t, vector<size_t> > cellspmat; // pair<kmer_id_j, vector<posix_in_read_i>>
typedef std::map< size_t, pair< vector<size_t>, vector<size_t> > > multcell; // map<kmer_id, vector<posix_in_read_i, posix_in_read_j>>

// Function to create the dictionary
// assumption: kmervect has unique entries
void dictionaryCreation(dictionary &kmerdict, Kmers &kmervect)
{
	for(size_t i = 0; i<kmervect.size(); i++)
  	{
        kmerdict.insert(make_pair(kmervect[i].rep(), i));
	}
	
}

int main (int argc, char* argv[]) {

	std::ifstream filein(argv[1]);
	FILE *fastafile;
	int elem;
	char *buffer;
	std::string kmerstr;
	std::string line;
	Kmer::set_k(KMER_LENGTH);
	dictionary kmerdict;
	std::vector<filedata> allfiles = GetFiles(argv[3]);
    size_t upperlimit = 1000; // 1000 reads at a time
    Kmer kmerfromstr;
    Kmers kmervect;
    std::vector<string> seqs;
    std::vector<string> quals;
    int rangeStart;
    Kmers kmersfromreads;
    std::vector<tuple<size_t,size_t,cellspmat>> occurrences;
    std::vector<tuple<size_t,size_t,cellspmat>> transtuples;

	cout << "Input file = " << argv[1] <<endl;
	cout << "Psbsim depth = 30" << endl;
	cout << "k-mer length = " << KMER_LENGTH <<endl;
	cout << "Reference genome = " << argv[2] <<endl;

    //automaticRange(filein, rangeStart);

	if(filein.is_open()) {
			while(getline(filein, line)) {
				if(line.length() == 0)
					break;

				string substring = line.substr(1);
				elem = stoi(substring);
				getline(filein, kmerstr); 	
				kmerfromstr.set_kmer(kmerstr.c_str());
				kmervect.push_back(kmerfromstr);								
			}
	} else std::cout << "unable to open the input file\n";
	filein.close();

	dictionaryCreation(kmerdict, kmervect);

    size_t read_id = 0; // read_id needs to be global (not just per file)
    for(auto itr=allfiles.begin(); itr!=allfiles.end(); itr++) {

        ParallelFASTQ *pfq = new ParallelFASTQ();
        pfq->open(itr->filename, false, itr->filesize);
        
        size_t fillstatus = 1;
        while(fillstatus) {

            fillstatus = pfq->fill_block(seqs, quals, upperlimit);
            size_t nreads = seqs.size();
            
            for(size_t i=0; i<nreads; i++) 
            {
            	// remember that the last valid position is length()-1
                size_t len = seqs[i].length();
                
                // skip this sequence if the length is too short
                if (len <= KMER_LENGTH)
                    continue;

                for(size_t j=0; j<=len-KMER_LENGTH; j++)  
                {

                    std::string kmerstrfromfastq = seqs[i].substr(j, KMER_LENGTH);
                    Kmer mykmer(kmerstrfromfastq.c_str());
                    // remember to use only ::rep() when building kmerdict as well
                    Kmer lexsmall = mykmer.rep();      

                    auto found = kmerdict.find(lexsmall);
                    if(found != kmerdict.end()) {
                    	occurrences.push_back(std::make_tuple(read_id, found->second, make_pair(found->second, vector<size_t>(1,j)))); // vector<tuple<read_id,kmer_id,pair<kmer_id,pos_in_read>>
                        transtuples.push_back(std::make_tuple(found->second, read_id, make_pair(found->second, vector<size_t>(1,j)))); // transtuples.push_back(col_id, row_id, value)
                    }
                }
                read_id++;
            }
        } 
	delete pfq;
    }
    cout << "Total number of reads = "<< read_id << endl;
    // cout << "fastq file parsed\nsearch ended : vector<tuple<read_id,kmer_id,pair<kmer_id,pos_in_read>> created" << endl;

    CSC<size_t, cellspmat> spmat(occurrences, read_id, kmervect.size(), 
                            [] (cellspmat & c1, cellspmat & c2) 
                            {   if(c1.first != c2.first) cout << "error in MergeDuplicates()" << endl;
                                vector<size_t> merged;
                                merge(c1.second.begin(), c1.second.end(), c2.second.begin(), c2.second.end(), back_inserter(merged));
                                return make_pair(c1.first, merged);
                            });
    std::cout << "spmat created" << endl;

    CSC<size_t, cellspmat> transpmat(transtuples, kmervect.size(), read_id, 
                            [] (cellspmat & c1, cellspmat & c2) 
                            {   if(c1.first != c2.first) cout << "error in MergeDuplicates()" << endl;
                                vector<size_t> merged;
                                merge(c1.second.begin(), c1.second.end(), c2.second.begin(), c2.second.end(), back_inserter(merged));
                                return make_pair(c1.first, merged);
                            });
    std::cout << "transpose(spmat) created" << endl;

    spmat.Sorted();
    transpmat.Sorted();

    double start = omp_get_wtime();
    CSC<size_t, multcell> tempspmat;

    HeapSpGEMM_gmalloc(spmat, transpmat, 
	    [] (cellspmat & c1, cellspmat & c2)
	    {	if(c1.first != c2.first) cout << "error in multop()" << endl;
            	multcell value;
            	pair<vector<size_t>, vector<size_t>> vectemp = make_pair(c1.second, c2.second);
            	value.insert(make_pair(c1.first, vectemp));
            	return value;
	    }, 
	    [] (multcell & h, multcell & m)
       	    {   m.insert(h.begin(), h.end());
           	return m;
       	    }, tempspmat);

	tempspmat.Apply();

	return 0;
} 

