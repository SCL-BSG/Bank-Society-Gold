
#include <mutex>
#include <condition_variable>

#define MAXIMUM_TID         30

#define THREAD_LOCK_CS_VNODES       1
#define THREAD_LOCK_CS_VSEND        2
#define THREAD_LOCK_CS_RXMSG        3
#define THREAD_LOCK_CS_VONESHOT     4
#define THREAD_LOCK_CS_ADDNODE      5
#define THREAD_LOCK_CS_SELSERVADDR  6

#define THREAD_LOCK_CS_MAIN         10
#define THREAD_LOCK_CS_INVENTORY    11

#define THREAD_LOCK_CS_MAPHOST      20

#define CURRENT_TASK                false
#define ALL_TASKS                   true

class Semaphore
{
public:    
    //Semaphore (int count_ = 0)
    //: count(count_)
    //{

    //}

    Semaphore ()
    {
        for ( task_ID = 1; task_ID < MAXIMUM_TID; task_ID++ )
        {
            count[ task_ID ] = -1;
        }
    }

    inline bool check (int tid )
    {

        if ( count[ task_ID ] > -1 )
            return true;
        else
            return false;

    }




    inline void notify( int tid, bool notify_all )
    {
        std::unique_lock<std::mutex> lock(mtx);
        //LogPrintf("\n\n Semaphore NOTIFY count %d Tid %d \n\n ", count[ tid ], tid );


        //cout << "thread " << tid <<  " notify" << endl;
        //notify the waiting thread
        if ( notify_all )
        {
            for ( task_ID = 1; task_ID < MAXIMUM_TID; task_ID++ )
            {
                //if ( count[ task_ID ] > 0 )
                //{
                    count[ task_ID ] = -1;
                    //LogPrintf("*** RGP SEM Notify One %d ", task_ID );
                    cv.notify_one();
                //}
            }
            //cv.notify_all();  RGP, it does not bloody work!!!
        }
        else
        {
            count[ tid ]--;
            //cout << "thread " << tid <<  " notify ONE"  << endl;
            cv.notify_one();
        }
    }

    inline bool wait( int tid )
    {
        if ( task_ID > MAXIMUM_TID  )
            return false;
        std::unique_lock<std::mutex> lock(mtx);
        count[ tid ]++;
        //LogPrintf("\n\n Semaphore WAIT count %d Tid %d \n\n ", count[ tid ], tid );
        while( count[ tid ] != 0)
        {
            //LogPrintf("\n\n Semaphore WAIT count %d Tid %d \n\n ", count[ tid ], tid );
            //cout << "thread " << tid << " wait" << endl;
            //wait on the mutex until notify is called
            cv.wait(lock);
            //cout << "thread " << tid << " run" << endl;
        }
        return true;

    }

private:
    std::mutex mtx;
    std::condition_variable cv;
    int count[ MAXIMUM_TID ];
    int task_ID;
};
