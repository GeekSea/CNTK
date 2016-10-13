//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full licence information.
//
#include "Include/Basics.h"
#include "Include/MPIWrapper.h"

#if HAS_OPENMPI
#pragma comment(lib, "msmpi.lib")
#else
#pragma warning(disable: 4100) // unreferenced formal parameter

#define MPI_SUCCESS           0
#define MPI_COMM_WORLD        0
#endif

#define FFLUSH_SUCCESS 0

namespace Microsoft { namespace MSR { namespace CNTK {

    // -----------------------------------------------------------------------
    // Generic MPIWrapper functions (not related to a specific implementation)
    // -----------------------------------------------------------------------

int operator||(int rc, const MpiFail &what)
{
    if (rc == MPI_SUCCESS)
    {
        return rc;
    }

    fprintf(stderr, "%s, MPI error %d\n", what.c_str(), rc);
    fflush(stderr);

#if HAS_OPENMPI
    // (special case: we use that code to indicate a missing msmpi.dll...)
    if (rc != MPI_ERR_INTERN)
    {
        char errbuf[MPI_MAX_ERROR_STRING + 1] = { 0 };
        int len;
        MPI_Error_string(rc, &errbuf[0], &len);
        fprintf(stderr, "%s, MPI error %d: %s\n", what.c_str(), rc, errbuf);
        fflush(stderr);

        // we abort through this, so that the MPI system gets the memo
        MPI_Abort(MPI_COMM_WORLD, rc);

        // TODO: or does that only signal an issue, and we should still terminate ourselves?
        // BUGBUG: We'd also need to Abort through the other sub-set communicator
    }
#endif

    RuntimeError("%s", what.c_str());
}

int MPIWrapperMpi::s_myRank = -1;
std::shared_ptr<MPIWrapper> MPIWrapperMpi::s_mpi = nullptr;

// Note that specifically, this function is such that it does not require
// MPI initialization. Moreover, it can be used without actually loading any
// MPI libs.
// TODO: Once we move to dynamic loading for MPI libs on Linux, move it to utilities.
int MPIWrapper::GetTotalNumberOfMPINodes()
{
#if !HAS_OPENMPI
    return 0;
#else
#ifdef WIN32
    const char* p = std::getenv("PMI_SIZE");
#else
    const char* p = std::getenv("OMPI_COMM_WORLD_SIZE");
#endif
    if (!p)
    {
        return 0;
    }
    else
    {
        return std::stoi(string(p));
    }
#endif
}

MPIWrapperPtr MPIWrapper::GetInstance(bool create)
{
    static bool initialized = false;
    if (create)
    {
        if (initialized)
            LogicError("Creating MPIWrapper instance after a GetInstance call has been already made!");
        else
            s_mpi = std::make_shared<MPIWrapperMpi>();
    }

    initialized = true;
    return s_mpi;
}

void MPIWrapper::DeleteInstance()
{
    s_mpi = nullptr;
}

// helpers to determine the MPI_Datatype of a pointer
MPI_Datatype MPIWrapper::GetDataType(char *)
{
    return MPI_CHAR;
}

MPI_Datatype MPIWrapper::GetDataType(int *)
{
    return MPI_INT;
}

MPI_Datatype MPIWrapper::GetDataType(float *)
{
    return MPI_FLOAT;
}

MPI_Datatype MPIWrapper::GetDataType(double *)
{
    return MPI_DOUBLE;
}

MPI_Datatype MPIWrapper::GetDataType(size_t *)
{
    return sizeof(size_t) == 4 ? MPI_UNSIGNED : MPI_LONG_LONG_INT;
}


    // -----------------------------------------------------------------------
    // MPIWrapper that actually calls into msmpi.dll
    // -----------------------------------------------------------------------

MPIWrapperMpi::MPIWrapperMpi()
    : m_currentComm(MPI_COMM_WORLD)
{
    static bool initialized = false;
    if (initialized)
    {
        LogicError("MPIWrapper: this is a singleton class that can only be instantiated once per process");
    }

    initialized = true;
    fprintf(stderr, "MPIWrapper: initializing MPI\n");
    fflush(stderr);

#if HAS_OPENMPI
    MPI_Init_DL() || MpiFail("mpiaggregator: MPI_Init");
    MPI_Comm_rank(MPI_COMM_WORLD, &m_myRank);
    MPI_Comm_size(MPI_COMM_WORLD, &m_numMPINodes);
#else
    m_myRank = 0;
    m_numMPINodes = 0;
#endif

    m_numNodesInUse = m_numMPINodes;

    // Verify that the environment variable used by GetTotalNumberOfMPINodes()  
    // matches what the MPI API says. There're actually two possible cases:
    // 1) when we're running with mpiexec both values have to match;
    // 2) when we're running without mpiexec, the former will return 0, and
    // the later will be set to 1.
    assert((GetTotalNumberOfMPINodes() == 0 && m_numNodesInUse == 1) ||
        (GetTotalNumberOfMPINodes() == m_numNodesInUse));

    // Applying MPI workaround
    s_myRank = m_myRank;
    atexit(&MPIWrapperMpi::MPIWorkaroundAtExit);

    // by default we use all of them
    RequestNodes("MPIWrapper");

    if (m_numMPINodes > 1)
        fprintf(stderr, "mpihelper: we are cog %d in a gearbox of %d\n", (int)m_myRank, (int)m_numMPINodes);
    else
        fprintf(stderr, "mpihelper: only one MPI process: MPI operation will be boring\n");

    fflush(stderr);

    // do an initial handshake
    Ping("mpihelper");

    // stagger the jobs just a little to get a sort-of deterministic order e.g. in GPU allocation when running on one machine
    // continue 0.5 seconds apart
    ::Sleep((DWORD)(500 * CurrentNodeRank()));
}

// Note: we don't clear the sub-communication here although we should, because in case of a crash, this prevents the EXE from terminating.
// It's OK since this class is a singleton anyway that gets instantiated exactly once at program startup.
MPIWrapperMpi::~MPIWrapperMpi()
{
    fprintf(stderr, "~MPIWrapper\n");

    // Do not finalize in event of an exception since calling MPI_Finalize without
    // all pending communications being finished results in a hang
    int rc = fflush(stderr);
    if (!std::uncaught_exception())
    {
        if (rc != FFLUSH_SUCCESS)
        {
#ifdef _WIN32
            RuntimeError("MPIWrapper: Failed to flush stderr, %d", ::GetLastError());
#else
            RuntimeError("MPIWrapper: Failed to flush stderr, %d", errno);
#endif
        }

        Finalize();
    }
}

// MPI_Init() with delay-loading the msmpi.dll (possibly causing a failure if missing; we want to catch that)
int MPIWrapperMpi::MPI_Init_DL()
{
#if !HAS_OPENMPI
    return MPI_SUCCESS;
#else
#ifdef WIN32
    __try
#endif
    {
        // don't initialize if that has been done already
        int flag = 0;
        MPI_Initialized(&flag);
        if (flag)
            return MPI_SUCCESS;

        int argc = 0;
        char **argv = NULL;
        int requiredThreadLevelSupport = MPI_THREAD_SERIALIZED;
        int provided;
        int ret = MPI_Init_thread(&argc, &argv, requiredThreadLevelSupport, &provided);
        if (provided != requiredThreadLevelSupport)
            LogicError("Failed to initialize MPI with the desired level of thread support");

        return ret;
    }
#ifdef WIN32
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        fprintf(stderr, "mpihelper: msmpi.dll missing\n");
        return MPI_ERR_INTERN;
    }
#endif
#endif
}

// Workaround for the issue with MPI hanging when we have non-0 exit codes from CNTK processes
// OpenMPI has a confirmed race condition on killing child process vs. handling their non-zero exit statuses, resulting
// in a deadlock, where all processes killed but MPI is still waiting.
// This happens when several perfectly synchronized processes (for example on MPI barrier)
// simulatenously exit with non-0 exit code.
// As a workaround, we simply sleep 50*rank miliseconds, effectively "de-synchronizing processes" at exit,
// allowing MPI to sequentially handle terminations
void MPIWrapperMpi::MPIWorkaroundAtExit()
{
    Sleep(s_myRank * 50);
}

void MPIWrapperMpi::Ping(const char *msg) const
{
#if HAS_OPENMPI
#undef USE2NDCOMM
#ifndef USE2NDCOMM
    if (NumNodesInUse() != m_numMPINodes)
    {
        fprintf(stderr, "ping [%s]: cannot be applied to subset (%d) of nodes, skipping\n", msg, (int)NumNodesInUse());
        fflush(stderr);
        return;
    }
#endif
    std::vector<int> handshake;
    handshake.push_back(1);

    fprintf(stderr, "ping [%s]: %d nodes pinging each other\n", msg, (int)NumNodesInUse());
    fflush(stderr);

    AllReduce(handshake);
    fprintf(stderr, "ping [%s]: all %d nodes responded\n", msg, handshake[0]);
    fflush(stderr);
#endif
}

void MPIWrapperMpi::RequestNodes(const char *msg, size_t requestednodes /*default: all*/)
{
#if HAS_OPENMPI
    Ping("requestnodes (before change)");

    // undo current split
#ifdef USE2NDCOMM
    if (m_currentComm != MPI_COMM_WORLD /*no subset*/ && m_currentComm != MPI_COMM_NULL /*idle nodes*/)
    {
        fprintf(stderr, "requestnodes: MPI_Comm_free %x\n", (int)m_currentComm);
        fflush(stderr);
        MPI_Comm_free(&m_currentComm) || MpiFail("requestnodes: MPI_Comm_free"); // will leave MPI_COMM_NULL here
    }
#endif
    // reset to MPI_COMM_WORLD
    m_currentComm = MPI_COMM_WORLD;
    // create a new split (unless all nodes were requested)
    if (requestednodes < (size_t)m_numMPINodes)
    {
#ifdef USE2NDCOMM
        fprintf(stderr, "requestnodes: MPI_Comm_split %d\n", (node() < requestednodes) ? 1 : MPI_UNDEFINED);
        fflush(stderr);
        MPI_Comm_split(communicator(), (node() < requestednodes) ? 1 : MPI_UNDEFINED, 0, &m_currentComm) || MpiFail("requestnodes: MPI_Comm_split");
        fprintf(stderr, "requestnodes: MPI_Comm_split -> %x\n", (int)m_currentComm);
        fflush(stderr);
#endif
    }
    else
    {
        // leave m_currentComm as MPI_COMM_WORLD
        // and clip to #nodes
        requestednodes = m_numMPINodes;
    }

    m_numNodesInUse = requestednodes;
    fprintf(stderr, "requestnodes [%s]: using %d out of %d MPI nodes (%d requested); we (%d) are %s\n",
        msg, (int)m_numNodesInUse, (int)m_numMPINodes, (int)requestednodes,
        (int)CurrentNodeRank(), IsIdle() ? "out (idle)" : "in (participating)");
    fflush(stderr);
    Ping("requestnodes (after change)");
#endif
}

MPI_Comm MPIWrapperMpi::Communicator() const
{
    return m_currentComm;
}

int MPIWrapperMpi::Finalize(void)
{
#if HAS_OPENMPI
    return MPI_Finalize();
#else
    return MPI_UNDEFINED;
#endif
}

// wait for all ranks to reach here
int MPIWrapperMpi::WaitAll()
{
#if HAS_OPENMPI
    return MPI_Barrier(m_currentComm) || MpiFail("waitall: MPI_Barrier");
#else
    return MPI_UNDEFINED;
#endif
}

int MPIWrapperMpi::Wait(MPI_Request* request, MPI_Status* status)
{
#if HAS_OPENMPI
    return MPI_Wait(request, status);
#else
    return MPI_UNDEFINED;
#endif
}

int MPIWrapperMpi::Waitany(int count, MPI_Request array_of_requests[], int* index, MPI_Status* status)
{
#if HAS_OPENMPI
    return MPI_Waitany(count, array_of_requests, index, status);
#else
    return MPI_UNDEFINED;
#endif
}

int MPIWrapperMpi::Waitall(int count, MPI_Request array_of_requests[], MPI_Status array_of_statuses[])
{
#if HAS_OPENMPI
    return MPI_Waitall(count, array_of_requests, array_of_statuses);
#else
    return MPI_UNDEFINED;
#endif
}

int MPIWrapperMpi::Isend(const void* buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Request* request)
{
#if HAS_OPENMPI
    return MPI_Isend(buf, count, datatype, dest, tag, m_currentComm, request);
#else
    return MPI_UNDEFINED;
#endif
}

int MPIWrapperMpi::Recv(void* buf, int count, MPI_Datatype datatype, int source, int tag, MPI_Status* status)
{
#if HAS_OPENMPI
    return MPI_Recv(buf, count, datatype, source, tag, m_currentComm, status);
#else
    return MPI_UNDEFINED;
#endif
}

int MPIWrapperMpi::Irecv(void* buf, int count, MPI_Datatype datatype, int source, int tag, MPI_Request* request)
{
#if HAS_OPENMPI
    return MPI_Irecv(buf, count, datatype, source, tag, m_currentComm, request);
#else
    return MPI_UNDEFINED;
#endif
}

int MPIWrapperMpi::Iallreduce(const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Request* request)
{
#if HAS_OPENMPI
    return MPI_Iallreduce(sendbuf, recvbuf, count, datatype, op, m_currentComm, request);
#else
    return MPI_UNDEFINED;
#endif
}

size_t MPIWrapperMpi::NumNodesInUse() const
{
    return m_numNodesInUse;
}

size_t MPIWrapperMpi::CurrentNodeRank() const
{
    return m_myRank;
}

bool MPIWrapperMpi::IsMainNode() const
{
    return m_myRank == 0;
} // we are the chosen one--do extra stuff like saving the model to disk

bool MPIWrapperMpi::IsIdle() const
{
    return CurrentNodeRank() >= NumNodesInUse();
} // user had requested to not use this many nodes

bool MPIWrapperMpi::UsingAllNodes() const
{
    return NumNodesInUse() == m_numMPINodes;
} // all nodes participate (used to check whether we can use MPI_Allreduce directly)

size_t MPIWrapperMpi::MainNodeRank() const
{
    return 0;
}

// allreduce of a vector
void MPIWrapperMpi::AllReduce(std::vector<size_t>&accumulator) const
{
#if HAS_OPENMPI
    auto *dataptr = accumulator.data();
    size_t totalnumelements = accumulator.size();

    // use MPI to compute the sum over all elements in (dataptr, totalnumelements) and redistribute to all nodes
    if ((NumNodesInUse() > 1) && (Communicator() != MPI_COMM_NULL))
    {
        MPI_Allreduce(MPI_IN_PLACE, dataptr, (int)totalnumelements, GetDataType(dataptr), MPI_SUM, Communicator()) || MpiFail("allreduce: MPI_Allreduce");
    }
#endif
}

void MPIWrapperMpi::AllReduce(std::vector<int>&accumulator) const
{
#if HAS_OPENMPI
    auto *dataptr = accumulator.data();
    size_t totalnumelements = accumulator.size();

    // use MPI to compute the sum over all elements in (dataptr, totalnumelements) and redistribute to all nodes
    if ((NumNodesInUse() > 1) && (Communicator() != MPI_COMM_NULL))
    {
        MPI_Allreduce(MPI_IN_PLACE, dataptr, (int)totalnumelements, GetDataType(dataptr), MPI_SUM, Communicator()) || MpiFail("allreduce: MPI_Allreduce");
    }
#endif
}

void MPIWrapperMpi::AllReduce(std::vector<double>&accumulator) const
{
#if HAS_OPENMPI
    auto *dataptr = accumulator.data();
    size_t totalnumelements = accumulator.size();

    // use MPI to compute the sum over all elements in (dataptr, totalnumelements) and redistribute to all nodes
    if ((NumNodesInUse() > 1) && (Communicator() != MPI_COMM_NULL))
    {
        MPI_Allreduce(MPI_IN_PLACE, dataptr, (int)totalnumelements, GetDataType(dataptr), MPI_SUM, Communicator()) || MpiFail("allreduce: MPI_Allreduce");
    }
#endif
}

void MPIWrapperMpi::AllReduce(std::vector<float>&accumulator) const
{
#if HAS_OPENMPI
    auto *dataptr = accumulator.data();
    size_t totalnumelements = accumulator.size();

    // use MPI to compute the sum over all elements in (dataptr, totalnumelements) and redistribute to all nodes
    if ((NumNodesInUse() > 1) && (Communicator() != MPI_COMM_NULL))
    {
        MPI_Allreduce(MPI_IN_PLACE, dataptr, (int)totalnumelements, GetDataType(dataptr), MPI_SUM, Communicator()) || MpiFail("allreduce: MPI_Allreduce");
    }
#endif
}

// for raw pointer
void MPIWrapperMpi::AllReduce(size_t*pData, size_t nData)
{
#if HAS_OPENMPI
    if ((NumNodesInUse() > 1 && (Communicator() != MPI_COMM_NULL)))
    {
        MPI_Allreduce(MPI_IN_PLACE, pData, (int)nData, GetDataType(pData), MPI_SUM, Communicator()) || MpiFail("Allreduce: MPI_Allreduce");
    }
#endif
}

void MPIWrapperMpi::AllReduce(int*pData, size_t nData)
{
#if HAS_OPENMPI
    if ((NumNodesInUse() > 1 && (Communicator() != MPI_COMM_NULL)))
    {
        MPI_Allreduce(MPI_IN_PLACE, pData, (int)nData, GetDataType(pData), MPI_SUM, Communicator()) || MpiFail("Allreduce: MPI_Allreduce");
    }
#endif
}

void MPIWrapperMpi::AllReduce(double*pData, size_t nData)
{
#if HAS_OPENMPI
    if ((NumNodesInUse() > 1 && (Communicator() != MPI_COMM_NULL)))
    {
        MPI_Allreduce(MPI_IN_PLACE, pData, (int)nData, GetDataType(pData), MPI_SUM, Communicator()) || MpiFail("Allreduce: MPI_Allreduce");
    }
#endif
}

void MPIWrapperMpi::AllReduce(float*pData, size_t nData)
{
#if HAS_OPENMPI
    if ((NumNodesInUse() > 1 && (Communicator() != MPI_COMM_NULL)))
    {
        MPI_Allreduce(MPI_IN_PLACE, pData, (int)nData, GetDataType(pData), MPI_SUM, Communicator()) || MpiFail("Allreduce: MPI_Allreduce");
    }
#endif
}

void MPIWrapperMpi::Bcast(size_t*pData, size_t nData, size_t srcRank)
{
#if HAS_OPENMPI
    if ((NumNodesInUse() > 1) && (Communicator() != MPI_COMM_NULL))
    {
        MPI_Bcast(pData, (int)nData, GetDataType(pData), (int)srcRank, Communicator()) || MpiFail("Bcast: MPI_Bcast");
    }
#endif
}

void MPIWrapperMpi::Bcast(double*pData, size_t nData, size_t srcRank)
{
#if HAS_OPENMPI
    if ((NumNodesInUse() > 1) && (Communicator() != MPI_COMM_NULL))
    {
        MPI_Bcast(pData, (int)nData, GetDataType(pData), (int)srcRank, Communicator()) || MpiFail("Bcast: MPI_Bcast");
    }
#endif
}

void MPIWrapperMpi::Bcast(float*pData, size_t nData, size_t srcRank)
{
#if HAS_OPENMPI
    if ((NumNodesInUse() > 1) && (Communicator() != MPI_COMM_NULL))
    {
        MPI_Bcast(pData, (int)nData, GetDataType(pData), (int)srcRank, Communicator()) || MpiFail("Bcast: MPI_Bcast");
    }
#endif
}

}}}
