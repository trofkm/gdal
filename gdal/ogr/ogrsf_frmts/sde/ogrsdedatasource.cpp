/******************************************************************************
 * $Id$
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRSDEDataSource class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2005, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2008, Shawn Gervais <project10@project10.net> 
 * Copyright (c) 2008, Howard Butler <hobu.inc@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "ogr_sde.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "gdal.h"

CPL_CVSID("$Id$");

/************************************************************************/
/*                          OGRSDEDataSource()                           */
/************************************************************************/

OGRSDEDataSource::OGRSDEDataSource()

{
    pszName = NULL;
    papoLayers = NULL;
    nLayers = 0;

    nState = SE_DEFAULT_STATE_ID;
    nNextState = -2;
    hConnection = NULL;
    hVersion = NULL;
}

/************************************************************************/
/*                          ~OGRSDEDataSource()                          */
/************************************************************************/

OGRSDEDataSource::~OGRSDEDataSource()

{
    int         i;
    LONG        nSDEErr;
    
    
    // Commit our transactions if we were opened for update
    if (bDSUpdate && bDSUseVersionEdits && (nNextState != -2 && nState != SE_DEFAULT_STATE_ID)) {
        CPLDebug("OGR_SDE", "Moving states from %d to %d", nState, nNextState);

        SE_connection_commit_transaction(hConnection);
        nSDEErr = SE_state_close(hConnection, nNextState);
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_state_close" );
        }
        nSDEErr = SE_version_change_state(hConnection, hVersion, nNextState);
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_version_change_state" );
        }
        nSDEErr = SE_state_trim_tree(hConnection, nState, nNextState);
        if( nSDEErr != SE_SUCCESS && nSDEErr != SE_STATE_INUSE)
        {
            if (nSDEErr != SE_STATE_INUSE) {
                IssueSDEError( nSDEErr, "SE_state_trim_tree" );
            }
        }
      

    }

    CPLFree( pszName );

    for( i = 0; i < nLayers; i++ )
        delete papoLayers[i];

    CPLFree( papoLayers );

    if (hVersion != NULL) 
    {
        SE_versioninfo_free(hVersion); 
    }   
    
    if( hConnection != NULL )
    {
        SE_connection_free( hConnection );
    }

 

}

/************************************************************************/
/*                           IssueSDEError()                            */
/************************************************************************/

void OGRSDEDataSource::IssueSDEError( int nErrorCode, 
                                      const char *pszFunction )

{
    char szErrorMsg[SE_MAX_MESSAGE_LENGTH+1];
    LONG nSDEErr;
    
    if( pszFunction == NULL )
        pszFunction = "SDE";
       
    if (bDSUpdate && bDSUseVersionEdits) {
        // try to clean up our state/transaction mess if we can
        nSDEErr = SE_state_delete(hConnection, nNextState);
        if (nSDEErr && nSDEErr != SE_STATE_INUSE) {
            SE_error_get_string( nSDEErr, szErrorMsg );
            CPLError( CE_Failure, CPLE_AppDefined, 
                      "SE_state_delete could not complete in IssueSDEError %d/%s", 
                      nErrorCode, szErrorMsg );
        }
        nSDEErr = SE_connection_rollback_transaction(hConnection);
        if (nSDEErr) {
            SE_error_get_string( nSDEErr, szErrorMsg );
            CPLError( CE_Failure, CPLE_AppDefined, 
                      "SE_connection_rollback_transaction could not complete in IssueSDEError %d/%s", 
                      nErrorCode, szErrorMsg );
        }
    }

    SE_error_get_string( nErrorCode, szErrorMsg );

    CPLError( CE_Failure, CPLE_AppDefined, 
              "%s: %d/%s", 
              pszFunction, nErrorCode, szErrorMsg );
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

int OGRSDEDataSource::Open( const char * pszNewName, int bUpdate )

{
    CPLAssert( nLayers == 0 );

/* -------------------------------------------------------------------- */
/*      If we aren't prefixed with SDE: then ignore this datasource.    */
/* -------------------------------------------------------------------- */
    if( !EQUALN(pszNewName,"SDE:",4) )
        return FALSE;

/* -------------------------------------------------------------------- */
/*      Parse arguments on comma.  We expect (layer is optional):       */
/*        SDE:server,instance,database,username,password,layer          */
/* -------------------------------------------------------------------- */
    char **papszTokens = CSLTokenizeStringComplex( pszNewName+4, ",",
                                                   TRUE, TRUE );

    CPLDebug( "OGR_SDE", "Open(\"%s\") revealed %d tokens.", pszNewName,
              CSLCount( papszTokens ) );

    if( CSLCount( papszTokens ) < 5 || CSLCount( papszTokens ) > 7 )
    {
        CPLError( CE_Failure, CPLE_OpenFailed, 
                  "SDE connect string had wrong number of arguments.\n"
                  "Expected 'SDE:server,instance,database,username,password,layer'\n"
		          "The layer name value is optional.\n"
                  "Got '%s'", 
                  pszNewName );
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Try to establish connection.                                    */
/* -------------------------------------------------------------------- */
    int 	nSDEErr;
    SE_ERROR    sSDEErrorInfo;

    nSDEErr = SE_connection_create( papszTokens[0], 
                                    papszTokens[1], 
                                    papszTokens[2], 
                                    papszTokens[3],
                                    papszTokens[4],
                                    &sSDEErrorInfo, &hConnection );

    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_connection_create" );
        return FALSE;
    }
    
    pszName = CPLStrdup( pszNewName );
    
    bDSUpdate = bUpdate;

    // Use SDE Versioned edits by default
    const char* pszVersionEdits;
    pszVersionEdits = CPLGetConfigOption( "SDE_VERSIONEDITS", "TRUE" );
    if (EQUAL(pszVersionEdits,"TRUE")) {
        bDSUseVersionEdits = TRUE;
    } else {
        bDSUseVersionEdits = FALSE;
    }

    
/* -------------------------------------------------------------------- */
/*      Set unprotected concurrency policy, suitable for single         */
/*      threaded access.                                                */
/* -------------------------------------------------------------------- */
    nSDEErr = SE_connection_set_concurrency( hConnection,
                                             SE_UNPROTECTED_POLICY);

    if( nSDEErr != SE_SUCCESS) {
        IssueSDEError( nSDEErr, NULL );
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Open a selected layer only, or else treat all known spatial     */
/*      tables as layers.                                               */
/* -------------------------------------------------------------------- */
    if ( CSLCount( papszTokens ) == 6 && *papszTokens[5] != '\0' )
    {
        OpenSpatialTable( papszTokens[5] );
    }
    else
    {
        EnumerateSpatialTables();
    }

/* -------------------------------------------------------------------- */
/*      Fetch the specified version or use SDE.DEFAULT if none is       */
/*      specified.                                                      */
/* -------------------------------------------------------------------- */

    if ( CSLCount( papszTokens ) == 7 && *papszTokens[6] != '\0' )
    {
        CPLDebug("OGR_SDE", "Setting version to %s", papszTokens[6]);
        nSDEErr = SetVersionState(papszTokens[6]);
        if (!nSDEErr)
        {
            // We've already set the error
            return FALSE;
        }        
    }
    else
    {
        CPLDebug("OGR_SDE", "Setting version to SDE.DEFAULT");
        nSDEErr = SetVersionState("SDE.DEFAULT");
        if (!nSDEErr)
        {
            // We've already set the error
            return FALSE;
        }        

    }
    CSLDestroy( papszTokens );
 
    return TRUE;
}

/************************************************************************/
/*                             SetVersionState()                        */
/************************************************************************/
int OGRSDEDataSource::SetVersionState( const char* pszVersionName ) {

    int nSDEErr;
    SE_STATEINFO hCurrentStateInfo= NULL;
    SE_STATEINFO hNextStateInfo= NULL;    
    SE_STATEINFO hDummyStateInfo = NULL;

    nSDEErr = SE_versioninfo_create(&hVersion);
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_versioninfo_create" );
        return FALSE;
    }
    nSDEErr = SE_version_get_info(hConnection, pszVersionName, hVersion);
    if( nSDEErr != SE_SUCCESS )
    {
        // this usually denotes incongruent versions of the client 
        // and server.  If this is the case, we're going to attempt to 
        // not do versioned queries at all.
        if ( nSDEErr == SE_INVALID_RELEASE ) {
            CPLDebug("OGR_SDE", "nState was set to SE_INVALID_RELEASE\n\n\n");
            // leave nState set to SE_DEFAULT_STATE_ID
            SE_versioninfo_free(hVersion);
            hVersion = NULL;
            IssueSDEError( nSDEErr, "SE_INVALID_RELEASE."
                           "  Your client/server versions must not match or " 
                           "you have some other major configuration problem");
            return FALSE;
            
        } else {
            IssueSDEError( nSDEErr, "SE_version_get_info" );
            return FALSE;
        }
    } 
    
    nSDEErr = SE_versioninfo_get_state_id(hVersion, &nState);

    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_versioninfo_get_state_id" );
        return FALSE;
    }
    
    if (bDSUpdate && bDSUseVersionEdits) {
        // So we're in update mode.  We need to get the state id 
        // of the active version, create a child state of it to 
        // push our edits onto, and close the state and move the 
        // version to it when we're done. 

        nSDEErr = SE_connection_start_transaction(hConnection);
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_connection_start_transaction" );
            return FALSE;
        } 
        nSDEErr = SE_stateinfo_create(&hCurrentStateInfo);
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_stateinfo_create" );
            return FALSE;
        }
        nSDEErr = SE_state_get_info(hConnection, nState, hCurrentStateInfo);
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_state_get_info" );
            return FALSE;
        }
        if (SE_stateinfo_is_open(hCurrentStateInfo)) {
                CPLError( CE_Failure, CPLE_AppDefined, 
                          "The editing state for this version is currently open.  "
                          "It must be closed for edits before it can be opened by OGR for update. ");
                return NULL;            
        }
        nSDEErr = SE_stateinfo_create(&hNextStateInfo);
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_stateinfo_create" );
            return FALSE;
        }
        
        nSDEErr = SE_stateinfo_create(&hDummyStateInfo);
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_stateinfo_create" );
            return FALSE;
        }
        nSDEErr = SE_state_create(hConnection, hDummyStateInfo, nState, hNextStateInfo);
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_state_create" );
            return FALSE;
        }

        nSDEErr = SE_stateinfo_get_id(hNextStateInfo, &nNextState);
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_stateinfo_get_id" );
            return FALSE;
        }
        nSDEErr = SE_state_open(hConnection, nNextState);
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_state_open" );
            return FALSE;
        }
        SE_stateinfo_free(hDummyStateInfo);
        SE_stateinfo_free(hCurrentStateInfo);
        SE_stateinfo_free(hNextStateInfo);

    }
    return TRUE; 


}
/************************************************************************/
/*                             OpenTable()                              */
/************************************************************************/

int OGRSDEDataSource::OpenTable( const char *pszTableName, 
                                 const char *pszFIDColumn,
                                 const char *pszShapeColumn,
                                 LONG nFIDColType )

{
/* -------------------------------------------------------------------- */
/*      Create the layer object.                                        */
/* -------------------------------------------------------------------- */
    OGRSDELayer  *poLayer;

    poLayer = new OGRSDELayer( this, bDSUpdate );

    if( !poLayer->Initialize( pszTableName, pszFIDColumn, pszShapeColumn ) )
    {
        delete poLayer;
        return FALSE;
    }
    
    poLayer->SetFIDColType( nFIDColType );

/* -------------------------------------------------------------------- */
/*      Add layer to data source layer list.                            */
/* -------------------------------------------------------------------- */
    papoLayers = (OGRSDELayer **)
        CPLRealloc( papoLayers,  sizeof(OGRSDELayer *) * (nLayers+1) );
    papoLayers[nLayers++] = poLayer;

    return TRUE;
}

/************************************************************************/
/*                            DeleteLayer()                             */
/************************************************************************/

OGRErr OGRSDEDataSource::DeleteLayer( int iLayer )

{
    if( iLayer < 0 || iLayer >= nLayers )
        return OGRERR_FAILURE;
    
/* -------------------------------------------------------------------- */
/*      Blow away our OGR structures related to the layer.  This is     */
/*      pretty dangerous if anything has a reference to this layer!     */
/* -------------------------------------------------------------------- */
    
    
    OGRSDELayer* poLayer = (OGRSDELayer*) papoLayers[iLayer];
    
    CPLString osGeometryName = poLayer->osShapeColumnName;
    CPLString osLayerName = poLayer->GetLayerDefn()->GetName();
    
    CPLDebug( "OGR_SDE", 
              "DeleteLayer(%s,%s)", 
              osLayerName.c_str(),
              osGeometryName.c_str() );

    delete papoLayers[iLayer];
    memmove( papoLayers + iLayer, papoLayers + iLayer + 1, 
             sizeof(void *) * (nLayers - iLayer - 1) );
    nLayers--;

/* -------------------------------------------------------------------- */
/*      Remove from the database.                                       */
/* -------------------------------------------------------------------- */
    LONG nSDEErr;
    char** paszTables;
    LONG nCount;
    char pszVersionName[SE_MAX_VERSION_LEN];
    
    nSDEErr = SE_layer_delete( hConnection, osLayerName.c_str(), osGeometryName.c_str());
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_layer_delete" );
        return OGRERR_FAILURE;
    }

    nSDEErr = SE_registration_get_dependent_tables(hConnection, osLayerName.c_str(), &paszTables, &nCount);
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_registration_get_dependent_tables" );
        return OGRERR_FAILURE;
    }

    for (int i=0; i<nCount; i++) {
        CPLDebug("OGR_SDE", "Dependent multiversion table: %s", paszTables[i]);
    }
    
    // if we still have dependent tables after deleting the layer, it is because the 
    // table is multiversion.  We need to smash the table to single version before 
    // deleting its registration.  If the user deletes the table from this version, 
    // all other versions are gone too.
    if (nCount) {
        nSDEErr = SE_versioninfo_get_name(hVersion, pszVersionName);
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_registration_make_single_version" );
            return OGRERR_FAILURE;
        }        
        nSDEErr = SE_registration_make_single_version(hConnection, pszVersionName, osLayerName.c_str());
        if( nSDEErr != SE_SUCCESS )
        {
            IssueSDEError( nSDEErr, "SE_registration_make_single_version" );
            return OGRERR_FAILURE;
        }
    }

    SE_registration_free_dependent_tables(paszTables, &nCount);

    nSDEErr = SE_registration_delete( hConnection, osLayerName.c_str() );
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_registration_delete" );
        return OGRERR_FAILURE;
    }
    
    nSDEErr = SE_table_delete( hConnection, osLayerName.c_str() );
    
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_table_delete" );
        return OGRERR_FAILURE;
    }
    
    CPLDebug( "OGR_SDE", "DeleteLayer(%s) successful", osLayerName.c_str() );

    return OGRERR_NONE;
}

/************************************************************************/
/*                            CreateLayer()                             */
/************************************************************************/

OGRLayer *
OGRSDEDataSource::CreateLayer( const char * pszLayerName,
                               OGRSpatialReference *poSRS,
                               OGRwkbGeometryType eType,
                               char ** papszOptions )

{
    LONG                nSDEErr;

/* -------------------------------------------------------------------- */
/*      Do we already have this layer?  If so, should we blow it        */
/*      away?                                                           */
/* -------------------------------------------------------------------- */
    int iLayer;


    // TODO: This is deficient. You can open a layer as 'tablename', but it
    // will be known as 'SDE.tablename' or whatever fully-qualified name.
    // This won't match in that case.

    for( iLayer = 0; iLayer < nLayers; iLayer++ )
    {

        if( EQUAL(pszLayerName,
                  papoLayers[iLayer]->GetLayerDefn()->GetName()) )
        {
            if( CSLFetchNameValue( papszOptions, "OVERWRITE" ) != NULL
                && !EQUAL(CSLFetchNameValue(papszOptions,"OVERWRITE"),"NO") )
            {
                DeleteLayer( iLayer );
            }
            else
            {
                CPLError( CE_Failure, CPLE_AppDefined, 
                          "Layer %s already exists, CreateLayer failed.\n"
                          "Use the layer creation option OVERWRITE=YES to "
                          "replace it.",
                          pszLayerName );
                return NULL;
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Get various layer creation options.                             */
/* -------------------------------------------------------------------- */
    const char         *pszExpectedFIDName;
    const char         *pszGeometryName;
    const char         *pszDbtuneKeyword;
    const char         *pszLayerDescription;

    pszExpectedFIDName = (char*) CPLMalloc(sizeof(char) * SE_MAX_COLUMN_LEN);
    pszGeometryName = (char*) CPLMalloc(sizeof(char) * SE_MAX_COLUMN_LEN);
    pszDbtuneKeyword = (char*) CPLMalloc(sizeof(char) * SE_MAX_CONFIG_KEYWORD_LEN);
    pszLayerDescription = (char*) CPLMalloc(sizeof(char) * 1024);

    pszGeometryName = CSLFetchNameValue( papszOptions, "GEOMETRY_NAME" );
    if( pszGeometryName == NULL )
        pszGeometryName = "SHAPE";

    
    pszExpectedFIDName = CPLGetConfigOption( "SDE_FID", "OBJECTID" );

    pszDbtuneKeyword = CSLFetchNameValue( papszOptions, "SDE_KEYWORD" );
    if( pszDbtuneKeyword == NULL )
        pszDbtuneKeyword = "DEFAULTS";
    
    // Set layer description
    pszLayerDescription = CSLFetchNameValue( papszOptions, "SDE_DESCRIPTION" );
    if( pszLayerDescription == NULL )
        pszLayerDescription = CPLSPrintf( "Created by GDAL/OGR %s", 
                                      GDALVersionInfo( "RELEASE_NAME" ) );

/* -------------------------------------------------------------------- */
/*      Create a basic table with the FID column                        */
/* -------------------------------------------------------------------- */
    SE_COLUMN_DEF       sColumnDef;
 
    /*
     * Setting the size and decimal_digits to 0 instructs SDE to
     * use default values for the SE_INTEGER_TYPE - these might be specific
     * to the underlying RDBMS.
     */
    strcpy( sColumnDef.column_name, pszExpectedFIDName );
    sColumnDef.sde_type       = SE_INTEGER_TYPE;
    sColumnDef.size           = 0;
    sColumnDef.decimal_digits = 0;
    sColumnDef.nulls_allowed  = FALSE;
    
    nSDEErr = SE_table_create( hConnection, pszLayerName, 1, &sColumnDef, 
                               pszDbtuneKeyword );
    
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_table_create" );
        return NULL;
    }


/* -------------------------------------------------------------------- */
/*      Convert the OGRSpatialReference to a SDE coordref object        */
/* -------------------------------------------------------------------- */
    SE_COORDREF         hCoordRef;
    
    if( ConvertOSRtoSDESpatRef( poSRS, &hCoordRef ) != OGRERR_NONE )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Cannot create layer %s: Unable to convert "
                  "OGRSpatialReference to SDE SE_COORDREF.",
                  pszLayerName );
        return NULL;
    }


/* -------------------------------------------------------------------- */
/*      Construct the layer info necessary to spatially enable          */
/*      the table.                                                      */
/* -------------------------------------------------------------------- */
    SE_LAYERINFO        hLayerInfo;
    SE_ENVELOPE         sLayerEnvelope;
    LONG                nLayerShapeTypes = 0L;
    
    nSDEErr = SE_layerinfo_create( hCoordRef, &hLayerInfo );
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_layerinfo_create" );
        return NULL;
    }
    
    // Determine the type of geometries that this layer will allow
    nLayerShapeTypes |= SE_NIL_TYPE_MASK;
    
    if( wkbFlatten(eType) == wkbPoint || wkbFlatten(eType) == wkbMultiPoint )
        nLayerShapeTypes |= SE_POINT_TYPE_MASK;
    
    else if( wkbFlatten(eType) == wkbLineString
             || wkbFlatten(eType) == wkbMultiLineString )
        nLayerShapeTypes |= ( SE_LINE_TYPE_MASK | SE_SIMPLE_LINE_TYPE_MASK );

    else if( wkbFlatten(eType) == wkbPolygon
             || wkbFlatten(eType) == wkbMultiPolygon )
        nLayerShapeTypes |= SE_AREA_TYPE_MASK;
    
    else if( eType == wkbUnknown )
    {
        nLayerShapeTypes |= (  SE_POINT_TYPE_MASK
                             | SE_LINE_TYPE_MASK 
                             | SE_SIMPLE_LINE_TYPE_MASK
                             | SE_AREA_TYPE_MASK );
        CPLError( CE_Warning, CPLE_AppDefined,
                  "Warning: Creation of a wkbUnknown layer in ArcSDE will "
                  "result in layers which are not displayable in Arc* "
                  "software" );
    }
    
    else
    {
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "Cannot create SDE layer %s with geometry type %d.",
                   pszLayerName, eType );
        
        SE_layerinfo_free( hLayerInfo );
        return NULL;
    }
    
    nSDEErr = SE_layerinfo_set_shape_types( hLayerInfo, nLayerShapeTypes );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_layerinfo_free( hLayerInfo );
        IssueSDEError( nSDEErr, "SE_layerinfo_set_shape_types" );
        return NULL;
    }
    
    
    // Set geometry column name
    nSDEErr = SE_layerinfo_set_spatial_column( hLayerInfo, pszLayerName,
                                               pszGeometryName );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_layerinfo_free( hLayerInfo );
        IssueSDEError( nSDEErr, "SE_layerinfo_set_spatial_column" );
        return NULL;
    }
    
    // Set creation keyword
    nSDEErr = SE_layerinfo_set_creation_keyword( hLayerInfo, pszDbtuneKeyword );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_layerinfo_free( hLayerInfo );
        IssueSDEError( nSDEErr, "SE_layerinfo_set_creation_keyword" );
        return NULL;
    }
    
    // Set layer extent based on coordinate system envelope
    if( poSRS != NULL && poSRS->IsGeographic() )
    {
        sLayerEnvelope.minx = -180;
        sLayerEnvelope.miny = -90;
        sLayerEnvelope.maxx =  180;
        sLayerEnvelope.maxy =  90;
    }
    else
    {
        nSDEErr = SE_coordref_get_xy_envelope( hCoordRef, &sLayerEnvelope );
        if( nSDEErr != SE_SUCCESS )
        {
            SE_layerinfo_free( hLayerInfo );
            IssueSDEError( nSDEErr, "SE_coordref_get_xy_envelope" );
            return NULL;
        }
    }
    
    nSDEErr = SE_layerinfo_set_envelope( hLayerInfo, &sLayerEnvelope );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_layerinfo_free( hLayerInfo );
        IssueSDEError( nSDEErr, "SE_layerinfo_set_envelope" );
        return NULL;
    }

    
    nSDEErr = SE_layerinfo_set_description( hLayerInfo, pszLayerDescription );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_layerinfo_free( hLayerInfo );
        IssueSDEError( nSDEErr, "SE_layerinfo_set_description" );
        return NULL;
    }
    
    
    // Set grid size
    nSDEErr = SE_layerinfo_set_grid_sizes( hLayerInfo,
                                           OGR_SDE_LAYER_CO_GRID1,
                                           OGR_SDE_LAYER_CO_GRID2,
                                           OGR_SDE_LAYER_CO_GRID3 );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_layerinfo_free( hLayerInfo );
        IssueSDEError( nSDEErr, "SE_layerinfo_set_grid_sizes" );
        return NULL;
    }
    
    
    // Set layer coordinate reference
    nSDEErr = SE_layerinfo_set_coordref( hLayerInfo, hCoordRef );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_layerinfo_free( hLayerInfo );
        IssueSDEError( nSDEErr, "SE_layerinfo_set_coordref" );
        return NULL;
    }
    
    
/* -------------------------------------------------------------------- */
/*      Spatially enable the newly created table                        */
/* -------------------------------------------------------------------- */
    nSDEErr = SE_layer_create( hConnection, hLayerInfo,
                               OGR_SDE_LAYER_CO_INIT_FEATS,
                               OGR_SDE_LAYER_CO_AVG_PTS );

    SE_layerinfo_free( hLayerInfo );
    
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_layer_create" );
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Register the newly created table                                */
/* -------------------------------------------------------------------- */
    char                szQualifiedTable[SE_QUALIFIED_TABLE_NAME];
    SE_REGINFO          hRegInfo;
    
    nSDEErr = SE_reginfo_create( &hRegInfo );
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_reginfo_create" );
        return NULL;
    }
    
    nSDEErr = SE_registration_get_info( hConnection, pszLayerName,
                                        hRegInfo );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_reginfo_free( hRegInfo );
        IssueSDEError( nSDEErr, "SE_registration_get_info" );
        return NULL;
    }
    
    nSDEErr = SE_reginfo_set_creation_keyword( hRegInfo, pszDbtuneKeyword );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_reginfo_free( hRegInfo );
        IssueSDEError( nSDEErr, "SE_reginfo_set_creation_keyword" );
        return NULL;
    }

    nSDEErr = SE_reginfo_set_rowid_column( hRegInfo, pszExpectedFIDName,
                                       SE_REGISTRATION_ROW_ID_COLUMN_TYPE_SDE );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_reginfo_free( hRegInfo );
        IssueSDEError( nSDEErr, "SE_reginfo_set_rowid_column" );
        return NULL;
    }
    
    /*
     * If the layer creation option 'MULTIVERSION' is set, enable
     * multi-versioning for this layer
     */
    if( CSLFetchBoolean( papszOptions, "SDE_MULTIVERSION", TRUE ) )
    {
        CPLDebug("OGR_SDE","Setting multiversion to true");
        nSDEErr = SE_reginfo_set_multiversion( hRegInfo, TRUE );
        if( nSDEErr != SE_SUCCESS )
        {
            SE_reginfo_free( hRegInfo );
            IssueSDEError( nSDEErr, "SE_reginfo_set_multiversion" );
            return NULL;
        }
    }
    
    nSDEErr = SE_registration_alter( hConnection, hRegInfo );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_reginfo_free( hRegInfo );
        IssueSDEError( nSDEErr, "SE_registration_alter" );
        return NULL;
    }
    
    nSDEErr = SE_reginfo_get_table_name( hRegInfo, szQualifiedTable );
    if( nSDEErr != SE_SUCCESS )
    {
        SE_reginfo_free( hRegInfo );
        IssueSDEError( nSDEErr, "SE_reginfo_get_table_name" );
        return NULL;
    }

    SE_reginfo_free( hRegInfo );
    SE_coordref_free( hCoordRef );
    
    
/* -------------------------------------------------------------------- */
/*      Create the layer object.                                        */
/* -------------------------------------------------------------------- */
    OGRSDELayer *poLayer;

    poLayer = new OGRSDELayer( this, bDSUpdate );
    
    if( !poLayer->Initialize( szQualifiedTable, pszExpectedFIDName,
                              pszGeometryName) )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Cannot initialize newly created layer \"%s\"",
                  pszLayerName );
        delete poLayer;
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Set various options on the layer.                               */
/* -------------------------------------------------------------------- */
    poLayer->SetFIDColType( SE_REGISTRATION_ROW_ID_COLUMN_TYPE_SDE );

/* -------------------------------------------------------------------- */
/*      Add layer to data source layer list.                            */
/* -------------------------------------------------------------------- */
    papoLayers = (OGRSDELayer **)
        CPLRealloc( papoLayers,  sizeof(OGRSDELayer *) * (nLayers+1) );
    
    papoLayers[nLayers++] = poLayer;

    CPLFree( (char*) pszExpectedFIDName );
    CPLFree( (char*) pszGeometryName );
    CPLFree( (char*) pszDbtuneKeyword );
    CPLFree( (char*) pszLayerDescription );

    return poLayer;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRSDEDataSource::TestCapability( const char * pszCap )

{
    if( EQUAL(pszCap,ODsCCreateLayer) && bDSUpdate )
        return TRUE;
    else if( EQUAL(pszCap,ODsCDeleteLayer) && bDSUpdate )
        return TRUE;
    else
        return FALSE;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRSDEDataSource::GetLayer( int iLayer )

{
    if( iLayer < 0 || iLayer >= nLayers )
        return NULL;
    else
        return papoLayers[iLayer];
}

/************************************************************************/
/*                       EnumerateSpatialTables()                       */
/************************************************************************/
void OGRSDEDataSource::EnumerateSpatialTables()
{
/* -------------------------------------------------------------------- */
/*      Fetch list of spatial tables from SDE.                          */
/* -------------------------------------------------------------------- */
    SE_REGINFO *ahTableList;
    LONG nTableListCount;
    LONG nSDEErr;

    nSDEErr = SE_registration_get_info_list( hConnection, &ahTableList,
                                             &nTableListCount );
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_registration_get_info_list" );
        return;
    }

    CPLDebug( "OGR_SDE", "SDE::EnumerateSpatialTables() found %d tables.", nTableListCount );

/* -------------------------------------------------------------------- */
/*      Process the tables, turning any appropriate ones into layers.   */
/* -------------------------------------------------------------------- */
    int iTable;

    for( iTable = 0; iTable < nTableListCount; iTable++ )
    {
        CreateLayerFromRegInfo( ahTableList[iTable] );
    }

    SE_registration_free_info_list( nTableListCount, ahTableList );
}

/************************************************************************/
/*                          OpenSpatialTable()                          */
/************************************************************************/

void OGRSDEDataSource::OpenSpatialTable( const char* pszTableName )
{
    SE_REGINFO tableinfo = NULL;
    LONG nSDEErr;

    CPLDebug( "OGR_SDE", "SDE::OpenSpatialTable(\"%s\").", pszTableName );

    nSDEErr = SE_reginfo_create( &tableinfo );
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_reginfo_create" );
    }

    nSDEErr = SE_registration_get_info( hConnection, pszTableName, tableinfo );
    if( nSDEErr != SE_SUCCESS )
    {
        IssueSDEError( nSDEErr, "SE_registration_get_info_list" );
    }
    else
    {
        CreateLayerFromRegInfo( tableinfo );
    }

    SE_reginfo_free( tableinfo );
}

/************************************************************************/
/*                       CreateLayerFromRegInfo()                       */
/************************************************************************/

void OGRSDEDataSource::CreateLayerFromRegInfo( SE_REGINFO& reginfo )
{
    char szTableName[SE_QUALIFIED_TABLE_NAME+1];
    char szIDColName[SE_MAX_COLUMN_LEN+1];
    LONG nFIDColType;
    LONG nSDEErr;

    // Ignore non-spatial, or hidden tables. 
    if( !SE_reginfo_has_layer( reginfo ) || SE_reginfo_is_hidden( reginfo ) )
    {
        return;
    }

    nSDEErr = SE_reginfo_get_table_name( reginfo, szTableName );
    if( nSDEErr != SE_SUCCESS )
    {
        return;
    }

    CPLDebug( "OGR_SDE", "CreateLayerFromRegInfo() asked to load table \"%s\".", szTableName );

    nSDEErr = SE_reginfo_get_rowid_column( reginfo, szIDColName, &nFIDColType );

    if( nFIDColType == SE_REGISTRATION_ROW_ID_COLUMN_TYPE_NONE
        || strlen(szIDColName) == 0 )
    {
        CPLDebug( "OGR_SDE", "Unable to determine FID column for %s.", 
                  szTableName );
        OpenTable( szTableName, NULL, NULL, nFIDColType );
    }
    else
        OpenTable( szTableName, szIDColName, NULL, nFIDColType );
}

/************************************************************************/
/*                       ConvertOSRtoSDESpatRef                         */
/************************************************************************/
OGRErr OGRSDEDataSource::ConvertOSRtoSDESpatRef( OGRSpatialReference *poSRS,
                                                 SE_COORDREF *psCoordRef )

{
    OGRSpatialReference  *poESRISRS;
    char                 *pszWkt;
    
    if( SE_coordref_create( psCoordRef ) != SE_SUCCESS )
        return OGRERR_FAILURE;
    
    // Construct a generic SE_COORDREF if poSRS is NULL
    if( poSRS == NULL )
    {
        SE_ENVELOPE     sGenericEnvelope;
        
        sGenericEnvelope.minx = -1000000;
        sGenericEnvelope.miny = -1000000;
        sGenericEnvelope.maxx =  1000000;
        sGenericEnvelope.maxy =  1000000;
        
        if( SE_coordref_set_xy_by_envelope( *psCoordRef, &sGenericEnvelope )
                != SE_SUCCESS )
        {
            SE_coordref_free( *psCoordRef );
            return OGRERR_FAILURE;
        }
        
        return OGRERR_NONE;
    }

    
    poESRISRS = poSRS->Clone();
    
    if( poESRISRS->morphToESRI() != OGRERR_NONE )
    {
        SE_coordref_free( *psCoordRef );
        delete poESRISRS;
        return OGRERR_FAILURE;
    }
    
    if( poESRISRS->exportToWkt( &pszWkt ) != OGRERR_NONE )
    {
        SE_coordref_free( *psCoordRef );
        delete poESRISRS;
        return OGRERR_FAILURE;
    }
    
    delete poESRISRS;
    
    if( SE_coordref_set_by_description( *psCoordRef, pszWkt ) != SE_SUCCESS )
        return OGRERR_FAILURE;
    
    CPLFree( pszWkt );
    
    return OGRERR_NONE;
}
