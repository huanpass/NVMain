/*******************************************************************************
* Copyright (c) 2012-2013, The Microsystems Design Labratory (MDL)
* Department of Computer Science and Engineering, The Pennsylvania State University
* All rights reserved.
* 
* This source code is part of NVMain - A cycle accurate timing, bit accurate
* energy simulator for both volatile (e.g., DRAM) and non-volatile memory
* (e.g., PCRAM). The source code is free and you can redistribute and/or
* modify it by providing that the following conditions are met:
* 
*  1) Redistributions of source code must retain the above copyright notice,
*     this list of conditions and the following disclaimer.
* 
*  2) Redistributions in binary form must reproduce the above copyright notice,
*     this list of conditions and the following disclaimer in the documentation
*     and/or other materials provided with the distribution.
* 
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
* 
* Author list: 
*   Matt Poremba    ( Email: mrp5060 at psu dot edu 
*                     Website: http://www.cse.psu.edu/~poremba/ )
*******************************************************************************/

#include "include/NVMHelpers.h"

namespace NVM {

int mlog2( int num )
{
    int retVal = -1;
    int newNum = num;

    if( num < 2 )
        return 0;

    while( newNum != 0 )
    {
        retVal++;
        newNum >>= 1;
    }

    return retVal;
}

std::string GetFilePath( std::string file )
{
    size_t last_sep;

    last_sep = file.find_last_of( "/\\" );
    
    return file.substr( 0, last_sep+1 );
} 

std::string PyDictHistogram( std::map<uint64_t, uint64_t> iiMap )
{

    /* Print a histogram as a python-style dict. */
    std::string pyHisto = "{";

    bool outputComma = false;
    for( std::map<uint64_t, uint64_t>::iterator iter = iiMap.begin();
         iter != iiMap.end(); ++iter )
    {
        if( outputComma )
            pyHisto += ", ";

        pyHisto += std::to_string(iter->first);
        pyHisto += ": ";
        pyHisto += std::to_string(iter->second);

        outputComma = true;
    }

    pyHisto += "}";

    return pyHisto;
}

};
