product netscape5
    id "Netscape Navigator 5.0b1 (1998 Mozilla Classic) for IRIX"
    image sw
        id "Netscape 5.0b1 Software"
        version VERSION
        order 9999
        subsys base default
            id "Netscape Navigator 5.0b1, with TLS (OpenSSL)"
            replaces self
            exp NETSCAPE5_BASE
            prereq (
                eoe.sw.base 1289434520 maxint
                x_eoe.sw.eoe 1289434520 maxint
                motif_eoe.sw.eoe 1289434520 maxint
            )
        endsubsys
    endimage
endproduct
