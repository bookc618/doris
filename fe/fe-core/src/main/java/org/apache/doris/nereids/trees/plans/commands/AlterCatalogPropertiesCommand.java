// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.nereids.trees.plans.commands;

import org.apache.doris.catalog.Env;
import org.apache.doris.common.util.PropertyAnalyzer;
import org.apache.doris.nereids.trees.plans.PlanType;
import org.apache.doris.nereids.trees.plans.visitor.PlanVisitor;
import org.apache.doris.qe.ConnectContext;
import org.apache.doris.qe.StmtExecutor;

import java.util.Map;
import java.util.Objects;

/**
 * Represents the command for ALTER CATALOG ... SET PROPERTIES.
 */
public class AlterCatalogPropertiesCommand extends AlterCatalogCommand {
    private final Map<String, String> newProperties;

    public AlterCatalogPropertiesCommand(String catalogName, Map<String, String> properties) {
        super(PlanType.ALTER_CATALOG_PROPERTIES_COMMAND, catalogName);
        this.newProperties = Objects.requireNonNull(properties, "Properties cannot be null");
    }

    @Override
    public void doRun(ConnectContext ctx, StmtExecutor executor) throws Exception {
        validate(ctx);
        PropertyAnalyzer.checkCatalogProperties(newProperties, true);

        Env.getCurrentEnv().getCatalogMgr().alterCatalogProps(catalogName, newProperties);
    }

    @Override
    public <R, C> R accept(PlanVisitor<R, C> visitor, C context) {
        return visitor.visitAlterCatalogPropertiesCommand(this, context);
    }
}
