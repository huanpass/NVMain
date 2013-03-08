/*******************************************************************************
* Copyright (c) 2012-2013, The Microsystems Design Labratory (MDL)
* Department of Computer Science and Engineering, The Pennsylvania State University
* All rights reserved.
* 
* This source code is part of NVMain - A cycle accurate timing, bit accurate
* energy simulator for both volatile (e.g., DRAM) and nono-volatile memory
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
*   Tao Zhang       ( Email: tzz106 at cse dot psu dot edu
*                     Website: http://www.cse.psu.edu/~tzz106 )
*******************************************************************************/

#include "src/MemoryController.h"
#include "include/NVMainRequest.h"
#include "src/EventQueue.h"

#include <cassert>
#include <cstdlib>
#include <csignal>

using namespace NVM;

MemoryController::MemoryController( )
{
    transactionQueues = NULL;
    memory = NULL;
    translator = NULL;

    starvationThreshold = 4;
    starvationCounter = NULL;
    activateQueued = NULL;
    effectiveRow = NULL;

    delayedRefreshCounter = NULL;
    
    curRank = 0;
    curBank = 0;
    nextRefreshRank = 0;
    nextRefreshBank = 0;
}

MemoryController::~MemoryController( )
{

    for( ncounter_t i = 0; i < p->RANKS; i++ )
    {
        delete [] bankQueues[i];
        delete [] starvationCounter[i];
        delete [] activateQueued[i];
        delete [] effectiveRow[i];
        delete [] bankNeedRefresh[i];
    }

    delete [] bankQueues;
    delete [] starvationCounter;
    delete [] activateQueued;
    delete [] effectiveRow;
    delete [] bankNeedRefresh;
    
    if( p->UseRefresh )
    {
        for( ncounter_t i = 0; i < p->RANKS; i++ )
        {
            /* Note: delete a NULL point is permitted in C++ */
            delete [] delayedRefreshCounter[i];
        }
    }

    delete [] delayedRefreshCounter;
    
}

MemoryController::MemoryController( Interconnect *memory, AddressTranslator *translator )
{
    this->memory = memory;
    this->translator = translator;

    transactionQueues = NULL;
}

AddressTranslator *MemoryController::GetAddressTranslator( )
{
    return translator;
}

void MemoryController::InitQueues( unsigned int numQueues )
{
    if( transactionQueues != NULL )
        delete [] transactionQueues;

    transactionQueues = new NVMTransactionQueue[ numQueues ];

    for( unsigned int i = 0; i < numQueues; i++ )
        transactionQueues[i].clear( );
}

void MemoryController::Cycle( ncycle_t steps )
{
    memory->Cycle( steps );
}

bool MemoryController::RequestComplete( NVMainRequest *request )
{
    if( request->type == REFRESH )
        ProcessRefreshPulse( request );
    else if( request->owner == this )
    {
        /* 
         *  Any activate/precharge/etc commands belong to the memory controller
         *  and we are in charge of deleting them!
         */
        delete request;
    }
    else
    {
        GetParent( )->RequestComplete( request );
    }

    return true;
}

bool MemoryController::QueueFull( NVMainRequest * /*request*/ )
{
    return false;
}

void MemoryController::SetMemory( Interconnect *mem )
{
    this->memory = mem;
}

Interconnect *MemoryController::GetMemory( )
{
    return (this->memory);
}

void MemoryController::SetTranslator( AddressTranslator *trans )
{
    this->translator = trans;
}

AddressTranslator *MemoryController::GetTranslator( )
{
    return (this->translator);
}

void MemoryController::SetConfig( Config *conf )
{
    this->config = conf;

    Params *params = new Params( );
    params->SetParams( conf );
    SetParams( params );
    
    translator->GetTranslationMethod( )->SetAddressMappingScheme( 
            p->AddressMappingScheme );
    
    bankQueues = new std::deque<NVMainRequest *> * [p->RANKS];
    starvationCounter = new unsigned int * [p->RANKS];
    activateQueued = new bool * [p->RANKS];
    effectiveRow = new uint64_t * [p->RANKS];

    for( ncounter_t i = 0; i < p->RANKS; i++ )
    {
        bankQueues[i] = new std::deque<NVMainRequest *> [p->BANKS];
        starvationCounter[i] = new unsigned int[p->BANKS];
        activateQueued[i] = new bool[p->BANKS];
        effectiveRow[i] = new uint64_t[p->BANKS];


        for( ncounter_t j = 0; j < p->BANKS; j++ )
        {
            starvationCounter[i][j] = 0;
            activateQueued[i][j] = false;
            /* set the initial effective row as invalid */
            effectiveRow[i][j] = p->ROWS;
        }
    }

    bankNeedRefresh = new bool * [p->RANKS];
    for( ncounter_t i = 0; i < p->RANKS; i++ )
    {
        bankNeedRefresh[i] = new bool[p->BANKS];
        for( ncounter_t j = 0; j < p->BANKS; j++ )
        {
            bankNeedRefresh[i][j] = false;
        }
    }
        
    delayedRefreshCounter = new unsigned * [p->RANKS];

    if( p->UseRefresh )
    {
        /* sanity check */
        assert( p->BanksPerRefresh <= p->BANKS );

        /* 
         * it does not make sense when refresh is needed 
         * but no bank can be refreshed 
         */
        assert( p->BanksPerRefresh != 0 );

        m_refreshBankNum = p->BANKS / p->BanksPerRefresh;
        
        /* first, calculate the tREFI */
        m_tREFI = p->tRFI / (p->ROWS / p->RefreshRows );

        /* then, calculate the time interval between two refreshes */
        ncycle_t m_refreshSlice = m_tREFI / ( p->RANKS * m_refreshBankNum );

        for( ncounter_t i = 0; i < p->RANKS; i++ )
        {
            delayedRefreshCounter[i] = new unsigned[m_refreshBankNum];
            
            /* initialize the counter to 0 */
            for( ncounter_t j = 0; j < m_refreshBankNum; j++ )
            {
                delayedRefreshCounter[i][j] = 0;

                uint64_t refreshBankHead = j * p->BanksPerRefresh;

                /* create first refresh pulse to start the refresh countdown */ 
                NVMainRequest* refreshPulse = MakeRefreshRequest( 
                                                0, 0, refreshBankHead, i );

                /* stagger the refresh */
                ncycle_t offset = (i * m_refreshBankNum + j ) * m_refreshSlice; 

                /* 
                 * insert refresh pulse, the event queue behaves like a 
                 * refresh countdown timer 
                 */
                GetEventQueue()->InsertEvent( EventResponse, this, refreshPulse, 
                        ( GetEventQueue()->GetCurrentCycle() + m_tREFI + offset ) );
            }
        }
    }

    this->config->Print();
}

/* 
 * NeedRefresh() has three functions:
 *  1) it returns false when no refresh is used (p->UseRefresh = false) 
 *  2) it returns false if the delayed refresh counter does not
 *  reach the threshold, which provides the flexibility for
 *  fine-granularity refresh 
 *  3) it automatically find the bank group the argument "bank"
 *  specifies and return the result
 */
bool MemoryController::NeedRefresh( const uint64_t bank, const uint64_t rank )
{
    bool rv = false;

    if( p->UseRefresh )
        if( delayedRefreshCounter[rank][bank/p->BanksPerRefresh] 
                >= p->DelayedRefreshThreshold )
            rv = true;
        
    return rv;
}

/* 
 * Set the refresh flag for a given bank group
 */
void MemoryController::SetRefresh( const uint64_t bank, const uint64_t rank )
{
    /* align to the head of bank group */
    uint64_t bankHead = ( bank / p->BanksPerRefresh ) * p->BanksPerRefresh;

    for( ncounter_t i = 0; i < p->BanksPerRefresh; i++ )
        bankNeedRefresh[rank][bankHead + i] = true;
}

/* 
 * Reset the refresh flag for a given bank group
 */
void MemoryController::ResetRefresh( const uint64_t bank, const uint64_t rank )
{
    /* align to the head of bank group */
    uint64_t bankHead = ( bank / p->BanksPerRefresh ) * p->BanksPerRefresh;

    for( ncounter_t i = 0; i < p->BanksPerRefresh; i++ )
        bankNeedRefresh[rank][bankHead + i] = false;
}

/* 
 * Increment the delayedRefreshCounter by 1 in a given bank group
 */
void MemoryController::IncrementRefreshCounter( const uint64_t bank, const uint64_t rank )
{
    /* get the bank group ID */
    uint64_t bankGroupID = bank / p->BanksPerRefresh;

    delayedRefreshCounter[rank][bankGroupID]++;
}

/* 
 * decrement the delayedRefreshCounter by 1 in a given bank group
 */
void MemoryController::DecrementRefreshCounter( const uint64_t bank, const uint64_t rank )
{
    /* get the bank group ID */
    uint64_t bankGroupID = bank / p->BanksPerRefresh;

    delayedRefreshCounter[rank][bankGroupID]--;
}

/* 
 * it simply checks all the banks in the refresh bank group whether their
 * command queues are empty. the result is the union of each check
 */
bool MemoryController::HandleRefresh( )
{
    for( ncounter_t rankIdx = 0; rankIdx < p->RANKS; rankIdx++ )
    {
        for( ncounter_t bankIdx = 0; bankIdx < m_refreshBankNum; bankIdx++ )
        {
            ncounter_t i = (nextRefreshRank + rankIdx) % p->RANKS;
            ncounter_t j = (nextRefreshBank + bankIdx * p->BanksPerRefresh) % p->BANKS;
            FailReason fail;

            if( NeedRefresh( j, i ) && IsRefreshBankQueueEmpty( j , i ) )
            {
                /* create a refresh command that will be sent to ranks */
                NVMainRequest* cmdRefresh = MakeRefreshRequest( 0, 0, j, i );

                if( !memory->IsIssuable( cmdRefresh, &fail ) )
                {
                    for( ncounter_t tmpBank = j; tmpBank < p->BanksPerRefresh; tmpBank++ ) 
                    {
                        /* Precharge all active banks and active subarrays */
                        if( activateQueued[i][tmpBank] == true && bankQueues[i][tmpBank].empty() )
                        {
                            /* issue a PRECHARGE_ALL command to close all subarrays */
                            bankQueues[i][tmpBank].push_back( 
                                    MakePrechargeAllRequest( 0, 0, tmpBank, i ) );

                            activateQueued[i][tmpBank] = false;
                            effectiveRow[i][tmpBank] = p->ROWS;
                        }
                        else
                        {
                            /* 
                             * TODO: PowerDown is not implemented, so we do nothing 
                             * PowerDown handling should be implemented later
                             */
                        }
                    }

                    /* delete the REFRESH command if it can not be issued */
                    delete cmdRefresh;

                    /* check next bank group */
                    continue;
                }

                /* 
                 * send the refresh command to the rank
                 * Note: some banks may be still open or powerdown. but we
                 * can send the REFRESH command since the extra POWERUP
                 * or PRECHARGE latency or even both have been counted
                 * in Bank.cc. See this file for the details.  
                 * Note: the cmdRefresh will be deleted in Rank.cpp
                 */
                cmdRefresh->issueCycle = GetEventQueue()->GetCurrentCycle();
                GetChild( )->IssueCommand( cmdRefresh );

                /* decrement the corresponding counter by 1 */
                DecrementRefreshCounter( j, i );

                /* if do not need refresh anymore, reset the refresh flag */
                if( !NeedRefresh( j, i ) )
                    ResetRefresh( j, i );

                /* round-robin */
                nextRefreshBank += p->BanksPerRefresh;
                if( nextRefreshBank >= p->BANKS )
                {
                    nextRefreshBank = 0;
                    nextRefreshRank++;

                    if( nextRefreshRank == p->RANKS )
                        nextRefreshRank = 0;
                }

                /* we should return since one time only one command can be issued */
                return true;  
            }
        }
    }
    return false;
}

/* 
 * it simply increments the corresponding delayed refresh counter 
 * and re-insert the refresh pulse into event queue
 */
void MemoryController::ProcessRefreshPulse( NVMainRequest* refresh )
{
    assert( refresh->type == REFRESH );

    uint64_t rank, bank;
    refresh->address.GetTranslatedAddress( NULL, NULL, &bank, &rank, NULL );

    IncrementRefreshCounter( bank, rank );

    if( NeedRefresh( bank, rank ) )
        SetRefresh( bank, rank ); 

    GetEventQueue()->InsertEvent( EventResponse, this, refresh, 
            ( GetEventQueue()->GetCurrentCycle() + m_tREFI ) );
}

/* 
 * it simply checks all banks in the refresh bank group whether their
 * command queues are empty. the result is the union of each check
 */
bool MemoryController::IsRefreshBankQueueEmpty( const uint64_t bank, const uint64_t rank )
{
    /* align to the head of bank group */
    uint64_t bankHead = ( bank / p->BanksPerRefresh ) * p->BanksPerRefresh;

    for( ncounter_t i = 0; i < p->BanksPerRefresh; i++ )
        if( !bankQueues[rank][bankHead + i].empty() )
            return false;

    return true;
}

Config *MemoryController::GetConfig( )
{
    return (this->config);
}

void MemoryController::SetID( unsigned int id )
{
    this->id = id;
}

NVMainRequest *MemoryController::MakeActivateRequest( NVMainRequest *triggerRequest )
{
    NVMainRequest *activateRequest = new NVMainRequest( );

    activateRequest->type = ACTIVATE;
    activateRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    activateRequest->address = triggerRequest->address;
    activateRequest->owner = this;

    return activateRequest;
}

NVMainRequest *MemoryController::MakeActivateRequest( const uint64_t row,
                                                      const uint64_t col,
                                                      const uint64_t bank,
                                                      const uint64_t rank )
{
    NVMainRequest *activateRequest = new NVMainRequest( );

    activateRequest->type = ACTIVATE;
    uint64_t actAddr = translator->ReverseTranslate( row, col, bank, rank, 0 );
    activateRequest->address.SetPhysicalAddress( actAddr );
    activateRequest->address.SetTranslatedAddress( row, col, bank, rank, 0 );
    activateRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    activateRequest->owner = this;

    return activateRequest;
}

NVMainRequest *MemoryController::MakePrechargeRequest( NVMainRequest *triggerRequest )
{
    NVMainRequest *prechargeRequest = new NVMainRequest( );

    prechargeRequest->type = PRECHARGE;
    prechargeRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    prechargeRequest->address = triggerRequest->address;
    prechargeRequest->owner = this;

    return prechargeRequest;
}

NVMainRequest *MemoryController::MakePrechargeRequest( const uint64_t row,
                                                       const uint64_t col,
                                                       const uint64_t bank,
                                                       const uint64_t rank )
{
    NVMainRequest *prechargeRequest = new NVMainRequest( );

    prechargeRequest->type = PRECHARGE;
    uint64_t preAddr = translator->ReverseTranslate( row, col, bank, rank, 0 );
    prechargeRequest->address.SetPhysicalAddress( preAddr );
    prechargeRequest->address.SetTranslatedAddress( row, col, bank, rank, 0 );
    prechargeRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    prechargeRequest->owner = this;

    return prechargeRequest;
}

NVMainRequest *MemoryController::MakePrechargeAllRequest( NVMainRequest *triggerRequest )
{
    NVMainRequest *prechargeAllRequest = new NVMainRequest( );

    prechargeAllRequest->type = PRECHARGE_ALL;
    prechargeAllRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    prechargeAllRequest->address = triggerRequest->address;
    prechargeAllRequest->owner = this;

    return prechargeAllRequest;
}

NVMainRequest *MemoryController::MakePrechargeAllRequest( const uint64_t row,
                                                          const uint64_t col,
                                                          const uint64_t bank,
                                                          const uint64_t rank )
{
    NVMainRequest *prechargeAllRequest = new NVMainRequest( );

    prechargeAllRequest->type = PRECHARGE_ALL;
    uint64_t preAddr = translator->ReverseTranslate( row, col, bank, rank, 0 );
    prechargeAllRequest->address.SetPhysicalAddress( preAddr );
    prechargeAllRequest->address.SetTranslatedAddress( row, col, bank, rank, 0 );
    prechargeAllRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    prechargeAllRequest->owner = this;

    return prechargeAllRequest;
}

NVMainRequest *MemoryController::MakeImplicitPrechargeRequest( NVMainRequest *triggerRequest )
{
    if( triggerRequest->type == READ )
        triggerRequest->type = READ_PRECHARGE;
    else if( triggerRequest->type == WRITE )
        triggerRequest->type = WRITE_PRECHARGE;

    triggerRequest->issueCycle = GetEventQueue()->GetCurrentCycle();

    return triggerRequest;
}

NVMainRequest *MemoryController::MakeRefreshRequest( const uint64_t row,
                                                     const uint64_t col,
                                                     const uint64_t bank,
                                                     const uint64_t rank )
{
    NVMainRequest *refreshRequest = new NVMainRequest( );

    refreshRequest->type = REFRESH;
    uint64_t preAddr = translator->ReverseTranslate( row, col, bank, rank, 0 );
    refreshRequest->address.SetPhysicalAddress( preAddr );
    refreshRequest->address.SetTranslatedAddress( row, col, bank, rank, 0 );
    refreshRequest->issueCycle = GetEventQueue()->GetCurrentCycle();
    refreshRequest->owner = this;

    return refreshRequest;
}

bool MemoryController::IsLastRequest( std::list<NVMainRequest *>& transactionQueue,
       uint64_t mRow, uint64_t mBank, uint64_t mRank )
{
    bool rv = true;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        uint64_t rank, bank, row;

        (*it)->address.GetTranslatedAddress( &row, NULL, &bank, &rank, NULL );

        /* if a request that has row buffer hit is found, return false */ 
        if( rank == mRank && bank == mBank && row == mRow )
        {
            rv = false;
            break;
        }
    }

    return rv;
}

bool MemoryController::FindStarvedRequest( std::list<NVMainRequest *>& transactionQueue, 
        NVMainRequest **starvedRequest )
{
    DummyPredicate pred;

    return FindStarvedRequest( transactionQueue, starvedRequest, pred );
}

bool MemoryController::FindStarvedRequest( std::list<NVMainRequest *>& transactionQueue, 
        NVMainRequest **starvedRequest, SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        uint64_t rank, bank, row;

        (*it)->address.GetTranslatedAddress( &row, NULL, &bank, &rank, NULL );

        if( activateQueued[rank][bank] && effectiveRow[rank][bank] != row    /* The effective row is not the row of this request. */
            && !bankNeedRefresh[rank][bank]                                  /* request is blocked when refresh is needed */
            && starvationCounter[rank][bank] >= starvationThreshold          /* This bank has reached it's starvation threshold. */
            && bankQueues[rank][bank].empty()                                /* No requests are currently issued to this bank. */
            && pred( row, bank, rank ) )                                     /* User-defined predicate is true. */
        {
            *starvedRequest = (*it);
            transactionQueue.erase( it );

            /* Different row buffer management policy has different behavior */ 

            /* 
             * if Relaxed Close-Page row buffer management policy is applied,
             * we check whether there is another request has row buffer hit.
             * if not, this request is the last request and we can close the
             * row.
             */
            if( p->ClosePage == 1 
                    && IsLastRequest( transactionQueue, row, bank, rank ) )
                (*starvedRequest)->tag = NVM_LASTREQUEST;
            /* 
             * else, if Restricted Close-Page is applied, the request is
             * always the last request
             */
            else if( p->ClosePage == 2 )
                (*starvedRequest)->tag = NVM_LASTREQUEST;

            rv = true;
            break;
        }
    }

    return rv;
}

bool MemoryController::FindRowBufferHit( std::list<NVMainRequest *>& transactionQueue, 
        NVMainRequest **hitRequest )
{
    DummyPredicate pred;

    return FindRowBufferHit( transactionQueue, hitRequest, pred );
}

bool MemoryController::FindRowBufferHit( std::list<NVMainRequest *>& transactionQueue, 
        NVMainRequest **hitRequest, SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        uint64_t rank, bank, row;

        (*it)->address.GetTranslatedAddress( &row, NULL, &bank, &rank, NULL );

        if( activateQueued[rank][bank] && effectiveRow[rank][bank] == row    /* The effective row is the row of this request. */
            && !bankNeedRefresh[rank][bank]                                  /* request is blocked when refresh is needed */
            && bankQueues[rank][bank].empty()                                /* No requests are currently issued to this bank. */
            && pred( row, bank, rank ) )                                     /* User-defined predicate is true. */
        {
            *hitRequest = (*it);
            transactionQueue.erase( it );

            /* Different row buffer management policy has different behavior */ 

            /* 
             * if Relaxed Close-Page row buffer management policy is applied,
             * we check whether there is another request has row buffer hit.
             * if not, this request is the last request and we can close the
             * row.
             */
            if( p->ClosePage == 1 
                    && IsLastRequest( transactionQueue, row, bank, rank ) )
                (*hitRequest)->tag = NVM_LASTREQUEST;
            /* 
             * else, if Restricted Close-Page is applied, the request is
             * always the last request
             */
            else if( p->ClosePage == 2 )
                (*hitRequest)->tag = NVM_LASTREQUEST;

            rv = true;

            break;
        }
    }

    return rv;
}

bool MemoryController::FindOldestReadyRequest( std::list<NVMainRequest *>& transactionQueue, 
        NVMainRequest **oldestRequest )
{
    DummyPredicate pred;

    return FindOldestReadyRequest( transactionQueue, oldestRequest, pred );
}

bool MemoryController::FindOldestReadyRequest( std::list<NVMainRequest *>& transactionQueue, 
        NVMainRequest **oldestRequest, SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        uint64_t rank, bank, row;

        (*it)->address.GetTranslatedAddress( &row, NULL, &bank, &rank, NULL );

        if( activateQueued[rank][bank] 
            && !bankNeedRefresh[rank][bank]    /* request is blocked when refresh is needed */
            && bankQueues[rank][bank].empty()  /* No requests are currently issued to this bank (Ready). */
            && pred( row, bank, rank ) )       /* User-defined predicate is true. */
        {
            *oldestRequest = (*it);
            transactionQueue.erase( it );
            
            /* Different row buffer management policy has different behavior */ 

            /* 
             * if Relaxed Close-Page row buffer management policy is applied,
             * we check whether there is another request has row buffer hit.
             * if not, this request is the last request and we can close the
             * row.
             */
            if( p->ClosePage == 1 
                    && IsLastRequest( transactionQueue, row, bank, rank ) )
                (*oldestRequest)->tag = NVM_LASTREQUEST;
            /* 
             * else, if Restricted Close-Page is applied, the request is
             * always the last request
             */
            else if( p->ClosePage == 2 )
                (*oldestRequest)->tag = NVM_LASTREQUEST;

            rv = true;
            break;
        }
    }

    return rv;
}

bool MemoryController::FindClosedBankRequest( std::list<NVMainRequest *>& transactionQueue, 
        NVMainRequest **closedRequest )
{
    DummyPredicate pred;

    return FindClosedBankRequest( transactionQueue, closedRequest, pred );
}

bool MemoryController::FindClosedBankRequest( std::list<NVMainRequest *>& transactionQueue, 
        NVMainRequest **closedRequest, SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        uint64_t rank, bank, row;

        (*it)->address.GetTranslatedAddress( &row, NULL, &bank, &rank, NULL );

        if( !activateQueued[rank][bank]          /* This bank is closed, anyone can issue. */
            && !bankNeedRefresh[rank][bank]      /* request is blocked when refresh is needed */
            && bankQueues[rank][bank].empty()    /* No requests are currently issued to this bank (Ready). */
            && pred( row, bank, rank ) )         /* User defined predicate is true. */
        {
            *closedRequest = (*it);
            transactionQueue.erase( it );
            
            /* Different row buffer management policy has different behavior */ 

            /* 
             * if Relaxed Close-Page row buffer management policy is applied,
             * we check whether there is another request has row buffer hit.
             * if not, this request is the last request and we can close the
             * row.
             */
            if( p->ClosePage == 1 
                    && IsLastRequest( transactionQueue, row, bank, rank ) )
                (*closedRequest)->tag = NVM_LASTREQUEST;
            /* 
             * else, if Restricted Close-Page is applied, the request is
             * always the last request
             */
            else if( p->ClosePage == 2 )
                (*closedRequest)->tag = NVM_LASTREQUEST;

            rv = true;
            break;
        }
    }

    return rv;
}

/*
 *  Slightly modify the scheduling functions form MemoryController.cpp to return a list instead
 *  of just a single request
 */
bool MemoryController::FindStarvedRequests( std::list<NVMainRequest *>& transactionQueue, 
        std::vector<NVMainRequest *>& starvedRequests )
{
    DummyPredicate pred;

    return FindStarvedRequests( transactionQueue, starvedRequests );
}

bool MemoryController::FindStarvedRequests( std::list<NVMainRequest *>& transactionQueue, 
        std::vector<NVMainRequest *>& starvedRequests, SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        uint64_t rank, bank, row;

        (*it)->address.GetTranslatedAddress( &row, NULL, &bank, &rank, NULL );

        if( activateQueued[rank][bank] && effectiveRow[rank][bank] != row    /* The effective row is not the row of this request. */
            && !bankNeedRefresh[rank][bank]                                  /* request is blocked when refresh is needed */
            && starvationCounter[rank][bank] >= starvationThreshold          /* This bank has reached it's starvation threshold. */
            && bankQueues[rank][bank].empty()                                /* No requests are currently issued to this bank. */
            && pred( row, bank, rank ) )                                     /* User-defined predicate is true. */
        {
            starvedRequests.push_back( (*it) );
            transactionQueue.erase( it );

            rv = true;
        }
    }

    return rv;
}

bool MemoryController::FindRowBufferHits( std::list<NVMainRequest *>& transactionQueue, 
        std::vector<NVMainRequest *>& hitRequests )
{
    DummyPredicate pred;

    return FindRowBufferHits( transactionQueue, hitRequests, pred );
}

bool MemoryController::FindRowBufferHits( std::list<NVMainRequest *>& transactionQueue, 
        std::vector<NVMainRequest* >& hitRequests, SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        uint64_t rank, bank, row;

        (*it)->address.GetTranslatedAddress( &row, NULL, &bank, &rank, NULL );

        if( activateQueued[rank][bank] && effectiveRow[rank][bank] == row    /* The effective row is the row of this request. */
            && !bankNeedRefresh[rank][bank]                                  /* request is blocked when refresh is needed */
            && bankQueues[rank][bank].empty()                                /* No requests are currently issued to this bank. */
            && pred( row, bank, rank ) )                                     /* User-defined predicate is true. */
        {
            hitRequests.push_back( (*it) );
            transactionQueue.erase( it );

            rv = true;
        }
    }

    return rv;
}

bool MemoryController::FindOldestReadyRequests( std::list<NVMainRequest *>& transactionQueue, 
        std::vector<NVMainRequest *> &oldestRequests )
{
    DummyPredicate pred;

    return FindOldestReadyRequests( transactionQueue, oldestRequests, pred );
}

bool MemoryController::FindOldestReadyRequests( std::list<NVMainRequest *>& transactionQueue, 
        std::vector<NVMainRequest *>& oldestRequests, SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        uint64_t rank, bank, row;

        (*it)->address.GetTranslatedAddress( &row, NULL, &bank, &rank, NULL );

        if( activateQueued[rank][bank] 
            && !bankNeedRefresh[rank][bank]    /* request is blocked when refresh is needed */
            && bankQueues[rank][bank].empty()  /* No requests are currently issued to this bank (Ready). */
            && pred( row, bank, rank ) )       /* User-defined predicate is true. */
        {
            oldestRequests.push_back( (*it) );
            transactionQueue.erase( it );
            
            rv = true;
        }
    }

    return rv;
}

bool MemoryController::FindClosedBankRequests( std::list<NVMainRequest *>& transactionQueue, 
        std::vector<NVMainRequest *> &closedRequests )
{
    DummyPredicate pred;

    return FindClosedBankRequests( transactionQueue, closedRequests, pred );
}

bool MemoryController::FindClosedBankRequests( std::list<NVMainRequest *>& transactionQueue, 
        std::vector<NVMainRequest *> &closedRequests, SchedulingPredicate& pred )
{
    bool rv = false;
    std::list<NVMainRequest *>::iterator it;

    for( it = transactionQueue.begin(); it != transactionQueue.end(); it++ )
    {
        uint64_t rank, bank, row;

        (*it)->address.GetTranslatedAddress( &row, NULL, &bank, &rank, NULL );

        if( !activateQueued[rank][bank]        /* This bank is closed, anyone can issue. */
            && !bankNeedRefresh[rank][bank]    /* request is blocked when refresh is needed */
            && bankQueues[rank][bank].empty()  /* No requests are currently issued to this bank (Ready). */
            && pred( row, bank, rank ) )       /* User defined predicate is true. */
        {
            closedRequests.push_back( (*it ) );
            transactionQueue.erase( it );
            
            rv = true;
        }
    }

    return rv;
}

bool MemoryController::DummyPredicate::operator() (uint64_t, uint64_t, uint64_t)
{
    return true;
}


bool MemoryController::IssueMemoryCommands( NVMainRequest *req )
{
    bool rv = false;
    uint64_t rank, bank, row;

    req->address.GetTranslatedAddress( &row, NULL, &bank, &rank, NULL );

    /*
     *  This function assumes the memory controller uses any predicates when
     *  scheduling. They will not be re-checked here.
     */

    if( !activateQueued[rank][bank] && bankQueues[rank][bank].empty() )
    {
        /* Any activate will request the starvation counter */
        starvationCounter[rank][bank] = 0;
        activateQueued[rank][bank] = true;
        effectiveRow[rank][bank] = row;

        req->issueCycle = GetEventQueue()->GetCurrentCycle();

        bankQueues[rank][bank].push_back( MakeActivateRequest( req ) );

        /* Different row buffer management policy has different behavior */ 
        /*
         * There are two possibilities that the request is the last request:
         * 1) ClosePage == 1 and there is no other request having row
         * buffer hit
         * or 2) ClosePage == 2, the request is always the last request
         */
        if( req->tag == NVM_LASTREQUEST )
        {
            bankQueues[rank][bank].push_back( MakeImplicitPrechargeRequest( req ) );
            activateQueued[rank][bank] = false;
            effectiveRow[rank][bank] = p->ROWS;
        }
        else
            bankQueues[rank][bank].push_back( req );

        rv = true;
    }
    else if( activateQueued[rank][bank] 
            && effectiveRow[rank][bank] != row 
            && bankQueues[rank][bank].empty() )
    {
        /* Any activate will request the starvation counter */
        starvationCounter[rank][bank] = 0;
        activateQueued[rank][bank] = true;

        req->issueCycle = GetEventQueue()->GetCurrentCycle();

        bankQueues[rank][bank].push_back( 
                MakePrechargeRequest( effectiveRow[rank][bank], 0, bank, rank ) );

        effectiveRow[rank][bank] = row;
        bankQueues[rank][bank].push_back( MakeActivateRequest( req ) );
        bankQueues[rank][bank].push_back( req );

        rv = true;
    }
    else if( activateQueued[rank][bank] && effectiveRow[rank][bank] == row )
    {
        starvationCounter[rank][bank]++;

        req->issueCycle = GetEventQueue()->GetCurrentCycle();

        /* Different row buffer management policy has different behavior */ 
        /*
         * There are two possibilities that the request is the last request:
         * 1) ClosePage == 1 and there is no other request having row
         * buffer hit
         * or 2) ClosePage == 2, the request is always the last request
         */
        if( req->tag == NVM_LASTREQUEST )
        {
            /* if Restricted Close-Page is applied, we should never be here */
            assert( p->ClosePage != 2 );

            bankQueues[rank][bank].push_back( MakeImplicitPrechargeRequest( req ) );
            activateQueued[rank][bank] = false;
            effectiveRow[rank][bank] = p->ROWS;
        }
        else
            bankQueues[rank][bank].push_back( req );

        rv = true;
    }
    else
    {
        rv = false;
    }

    return rv;
}

void MemoryController::CycleCommandQueues( )
{
    /* First of all, see whether we can issue a necessary refresh */
    if( p->UseRefresh ) 
    {
        if( HandleRefresh() )
            return;
    }

    for( ncounter_t rankIdx = 0; rankIdx < p->RANKS; rankIdx++ )
    {
        for( ncounter_t bankIdx = 0; bankIdx < p->BANKS; bankIdx++ )
        {
            ncounter_t i = (curRank + rankIdx)%p->RANKS;
            ncounter_t j = (curBank + bankIdx)%p->BANKS;
            FailReason fail;

            if( !bankQueues[i][j].empty( )
                && memory->IsIssuable( bankQueues[i][j].at( 0 ), &fail ) )
            {
                GetChild( )->IssueCommand( bankQueues[i][j].at( 0 ) );

                bankQueues[i][j].erase( bankQueues[i][j].begin( ) );

                MoveRankBank();

                /* we should return since one time only one command can be issued */
                return ;
            }
            else if( !bankQueues[i][j].empty( ) )
            {
                NVMainRequest *queueHead = bankQueues[i][j].at( 0 );

                if( ( GetEventQueue()->GetCurrentCycle() - queueHead->issueCycle ) > 1000000 )
                {
                    uint64_t bank, rank, channel;
                    queueHead->address.GetTranslatedAddress( NULL, NULL, &bank, &rank, &channel );
                    std::cout << "NVMain Warning: Operation could not be sent to memory after a very long time: "
                              << std::endl; 
                    std::cout << "         Address: 0x" << std::hex 
                              << queueHead->address.GetPhysicalAddress( )
                              << std::dec << " @ Bank " << bank << ", Rank " << rank << ", Channel " << channel
                              << ". Queued time: " << queueHead->arrivalCycle
                              << ". Current time: " << GetEventQueue()->GetCurrentCycle() << ". Type: " 
                              << queueHead->type << std::endl;

                    // Give the opportunity to attach a debugger here.
                    raise( SIGSTOP );
                    exit(1);
                }
            }
        }
    }
}

/* 
 * MoveRankBank() increment curRank and/or curBank according to the scheduling
 * scheme
 * 0 -- Fixed Scheduling from Rank0 and Bank0
 * 1 -- Rank-first round-robin
 * 2 -- Bank-first round-robin
 */
void MemoryController::MoveRankBank( )
{
    if( p->ScheduleScheme == 1 )
    {
        /* increment Rank. if all ranks are looked, increment Bank then */
        curRank++;
        if( curRank == p->RANKS )
        {
            curRank = 0;
            curBank = (curBank + 1)%p->BANKS;
        }
    }
    else if( p->ScheduleScheme == 2 )
    {
        /* increment Bank. if all banks are looked, increment Rank then */
        curBank++;
        if( curBank == p->BANKS )
        {
            curBank = 0;
            curRank = (curRank + 1)%p->RANKS;
        }
    }

    /* if fixed scheduling is used, we do nothing */
}

void MemoryController::PrintStats( )
{
    memory->PrintStats( );
    translator->PrintStats( );
}
