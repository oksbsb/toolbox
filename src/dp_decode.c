
 * \brief Return a malloced packet.
 */
void PacketFree(Packet *p)
{
    p = p;
}


 
 
 
 


    
    
return NULL;
    }
memset(p, 0, DEFAULT_PACKET_SIZE);
    //PACKET_INITIALIZE(p);
    p->flags |= PKT_ALLOC;

    //PACKET_PROFILING_START(p);
    return p;


